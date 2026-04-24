"""
主界面模块。

本版面向教育和创客场景，采用明亮友好的卡片式布局：
1. 左侧为分步标签页式控制面板，按“连接 -> 创作 -> 雕刻”组织工作流；
2. 右侧为展示区，上方看图、下方看消息；
3. 保留 AI 生图、本地图导入、G-Code 生成、设备发送的完整闭环；
4. 比赛版可在同一界面切换“串口 UART”或“WiFi TCP”两种下发方式。
"""

from __future__ import annotations

import html
import json
import shutil
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .ai_image_generator import AIGeneratorThread, DEFAULT_MODEL_LABEL, GeneratedImageResult
from .gcode_generator import (
    BOARD_WORK_AREA_X_MM,
    BOARD_WORK_AREA_Y_MM,
    generate_gcode,
    generate_preview_gcode,
    save_gcode_file,
)
from .image_processing import ImageProcessingError, NormalizedPath, process_image_to_contours
from .prompt_beautifier import PromptOptimizerThread
from .qt_compat import QT_API, QtCore, QtGui, QtWidgets
from .serial_worker import SerialWorkerThread
from .task_manager import TaskManager, TaskState


BRIGHT_MAKER_QSS = """
QWidget {
    background-color: #F5F7FB;
    color: #2D3748;
    font-family: "Microsoft YaHei", "PingFang SC", sans-serif;
    font-size: 14px;
}
QFrame#Card {
    background-color: #FFFFFF;
    border: 1px solid #DCE3EC;
    border-radius: 16px;
    margin: 4px;
}
QFrame#Card[previewRole="floating"][previewCollapsed="true"] {
    background-color: #FFFFFF;
    border: 1px solid #E6EDF5;
    border-radius: 14px;
}
QFrame#Card[previewRole="floating"][previewCollapsed="true"][previewHover="true"] {
    background-color: #F8FBFE;
    border: 1px solid #DCE7F2;
}
QFrame#StatusCard {
    background-color: #FFFFFF;
    border: 1px solid #DCE3EC;
    border-radius: 16px;
    margin: 4px;
}
QLabel { background-color: transparent; font-weight: bold; color: #4A5568; }
QLabel#TaskStatusLabel {
    color: #2D3748;
    font-size: 13px;
    font-weight: 700;
    padding-left: 6px;
}
QLabel#TaskDetailLabel {
    color: #5F6F82;
    font-size: 11px;
    font-weight: 400;
}

QPushButton {
    background-color: #FFFFFF;
    border: 1px solid #D6DCE6;
    border-radius: 10px;
    padding: 7px 14px;
    min-height: 34px;
    font-weight: bold;
    color: #4A5568;
}
QPushButton:hover { border-color: #9FB3C8; background-color: #F7FAFD; }
QPushButton:pressed { background-color: #EDF2F7; }
QPushButton:disabled {
    background-color: #E2E8F0;
    color: #A0AEC0;
    border-color: #E2E8F0;
}

QPushButton#BtnAI {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #667EEA, stop:1 #764BA2);
    color: #FFFFFF;
    border: none;
}
QPushButton#BtnAI:hover {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #764BA2, stop:1 #667EEA);
    color: #FFFFFF;
}

QPushButton#BtnLocal {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ED8936, stop:1 #DD6B20);
    color: #FFFFFF;
    border: none;
}
QPushButton#BtnLocal:hover { background-color: #C05621; color: #FFFFFF; }

QPushButton#BtnStart {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #48BB78, stop:1 #38A169);
    color: #FFFFFF;
    border: none;
}
QPushButton#BtnStart:hover { background-color: #2F855A; color: #FFFFFF; }

QPushButton#BtnStop { background-color: #F56565; color: #FFFFFF; border: none; }
QPushButton#BtnStop:hover { background-color: #C53030; color: #FFFFFF; }
QPushButton#BtnPromptAssist {
    background-color: #F8FBFF;
    border: 1px solid #C9D9EC;
    color: #2F6FB3;
}
QPushButton#BtnPromptAssist:hover {
    background-color: #EEF5FF;
    border-color: #AFC8E5;
    color: #235A93;
}
QPushButton#BtnPromptAssist:pressed {
    background-color: #E3EEFC;
    border-color: #9BB7D8;
}
QPushButton#BtnPromptAssist:disabled {
    background-color: #F3F6FA;
    color: #9AA9B8;
    border-color: #D8E0E8;
}

QPushButton#BtnPrimary {
    background-color: #409EFF;
    border: none;
    color: #FFFFFF;
}
QPushButton#BtnPrimary:hover {
    background-color: #2F8CF0;
    color: #FFFFFF;
}
QPushButton#BtnPrimary:pressed {
    background-color: #247CDB;
}
QPushButton[secondary="true"] {
    background-color: #F8FAFC;
    border: 1px solid #D9E2EC;
    color: #4A5568;
    padding: 6px 12px;
}
QPushButton[secondary="true"]:hover {
    background-color: #EEF4FA;
    border-color: #B9C7D8;
}

QLineEdit, QTextEdit {
    background-color: #F7FAFC;
    border: 1px solid #E2E8F0;
    border-radius: 8px;
    padding: 8px;
    color: #2D3748;
}
QLineEdit:focus, QTextEdit:focus {
    border: 2px solid #667EEA;
    background-color: #FFFFFF;
}

QComboBox {
    background-color: #F7FAFC;
    border: 1px solid #E2E8F0;
    border-radius: 8px;
    padding: 8px 40px 8px 12px;
    color: #2D3748;
}
QComboBox:focus {
    border: 2px solid #667EEA;
    background-color: #FFFFFF;
}
QComboBox::drop-down {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 32px;
    border-left: 1px solid #E2E8F0;
    border-top-right-radius: 8px;
    border-bottom-right-radius: 8px;
    background-color: #F8FAFC;
}
QComboBox::drop-down:hover {
    background-color: #E2E8F0;
}
QComboBox::drop-down:pressed {
    background-color: #CBD5E0;
}
QComboBox::down-arrow {
    image: url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 24 24' fill='none' stroke='%234A5568' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><polyline points='6 9 12 15 18 9'/></svg>");
    width: 12px;
    height: 12px;
}
QComboBox::drop-down:pressed > QComboBox::down-arrow {
    margin-top: 1px;
}

QSpinBox, QDoubleSpinBox {
    background-color: #F7FAFC;
    border: 1px solid #E2E8F0;
    border-radius: 8px;
    padding: 8px 40px 8px 12px;
    color: #2D3748;
    min-height: 22px;
}
QSpinBox:focus, QDoubleSpinBox:focus {
    border: 2px solid #667EEA;
    background-color: #FFFFFF;
}

QSpinBox::up-button, QDoubleSpinBox::up-button {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 32px;
    border-left: 1px solid #E2E8F0;
    border-bottom: 1px solid #E2E8F0;
    border-top-right-radius: 8px;
    background-color: #F8FAFC;
}

QSpinBox::down-button, QDoubleSpinBox::down-button {
    subcontrol-origin: border;
    subcontrol-position: bottom right;
    width: 32px;
    border-left: 1px solid #E2E8F0;
    border-bottom-right-radius: 8px;
    background-color: #F8FAFC;
}

QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
    background-color: #E2E8F0;
}
QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed,
QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {
    background-color: #CBD5E0;
}

QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    image: url("data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIxNCIgaGVpZ2h0PSIxNCIgdmlld0JveD0iMCAwIDI0IDI0IiBmaWxsPSJub25lIiBzdHJva2U9IiM0QTU1NjgiIHN0cm9rZS13aWR0aD0iMyIgc3Ryb2tlLWxpbmVjYXA9InJvdW5kIiBzdHJva2UtbGluZWpvaW49InJvdW5kIj48cG9seWxpbmUgcG9pbnRzPSIxOCAxNSAxMiA5IDYgMTUiLz48L3N2Zz4=");
    width: 14px;
    height: 14px;
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    image: url("data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIxNCIgaGVpZ2h0PSIxNCIgdmlld0JveD0iMCAwIDI0IDI0IiBmaWxsPSJub25lIiBzdHJva2U9IiM0QTU1NjgiIHN0cm9rZS13aWR0aD0iMyIgc3Ryb2tlLWxpbmVjYXA9InJvdW5kIiBzdHJva2UtbGluZWpvaW49InJvdW5kIj48cG9seWxpbmUgcG9pbnRzPSI2IDkgMTIgMTUgMTggOSIvPjwvc3ZnPg==");
    width: 14px;
    height: 14px;
}
QSpinBox::up-button:pressed > .up-arrow, QDoubleSpinBox::up-button:pressed > .up-arrow {
    margin-top: 1px;
}
QSpinBox::down-button:pressed > .down-arrow, QDoubleSpinBox::down-button:pressed > .down-arrow {
    margin-top: 1px;
}

QTextEdit#LogBox {
    background-color: transparent;
    border: none;
    border-radius: 8px;
    color: #34495E;
    font-family: Consolas, monospace;
}

QTextEdit#PromptBox {
    font-weight: normal;
}

QLabel#CardTitle {
    font-size: 16px;
    color: #2D3748;
    padding-top: 2px;
}

QLabel#CardHint {
    font-size: 11px;
    font-weight: normal;
    color: #718096;
    padding-bottom: 2px;
}

QLabel#CompactHint {
    font-size: 11px;
    font-weight: normal;
    color: #718096;
}

QLabel#PreviewTitle {
    font-size: 13px;
    color: #4A5568;
}

QFrame#PreviewPanel {
    background-color: #F8F9FA;
    border: 1px solid #F1F4F7;
    border-radius: 12px;
}
QFrame#HistoryCard {
    background-color: #FFFFFF;
    border: 1px solid #E6EDF5;
    border-radius: 14px;
}
QFrame#HistoryCard:hover {
    border-color: #C8D6E5;
    background-color: #FBFDFF;
}
QLabel#HistoryMeta {
    color: #7F8C9A;
    font-size: 12px;
    font-weight: normal;
}
QLabel#HistoryEmptyState {
    color: #7F8C9A;
    font-size: 13px;
    font-weight: normal;
    padding: 12px;
}

QWidget#PreviewFloatingBar {
    background: transparent;
}

QFrame#LiveTelemetryStrip {
    background-color: #F8FBFE;
    border: 1px solid #DCE3EC;
    border-radius: 12px;
}

QLabel#LiveTelemetryLabel {
    font-size: 11px;
    font-weight: normal;
    color: #718096;
    padding: 1px 2px;
}

QLabel#LiveTelemetryValue {
    font-size: 14px;
    font-weight: 800;
    color: #2D3748;
    padding: 1px 2px;
}
QLabel#StatusInlineKey {
    color: #2F6FB3;
    font-size: 11px;
    font-weight: 700;
}
QLabel#StatusInlineValue {
    color: #2D3748;
    font-size: 11px;
    font-weight: 700;
}
QLabel#StatusInlineSeparator {
    color: #A0AEC0;
    font-size: 11px;
    font-weight: 400;
    padding: 0 1px;
}

QFrame#LeftFillPanel {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #EFF4FB, stop:1 #E7EEF8);
    border: 1px solid #D6E2F1;
    border-radius: 16px;
    margin: 4px;
}

QLabel#LeftFillTitle {
    font-size: 13px;
    font-weight: 800;
    color: #355070;
}

QLabel#LeftFillText {
    font-size: 12px;
    font-weight: normal;
    color: #5F7288;
}

QToolButton#PreviewToggleButton {
    background-color: transparent;
    border: none;
    border-radius: 11px;
    color: rgba(65, 90, 119, 92);
    min-width: 24px;
    max-width: 24px;
    min-height: 24px;
    max-height: 24px;
}

QToolButton#PreviewToggleButton:hover {
    background-color: rgba(237, 244, 251, 220);
}

QToolButton#PreviewToggleButton[active="true"] {
    background-color: rgba(247, 250, 253, 235);
    color: #415A77;
}

QFrame#PreviewCompactBar {
    background-color: transparent;
    border: none;
}

QLabel#PreviewCompactTag {
    background-color: transparent;
    border: none;
    padding: 0;
}

QLabel#PreviewCompactSummary {
    font-size: 12px;
    font-weight: normal;
    color: #526072;
}

QLabel#PreviewCompactCaption {
    font-size: 11px;
    font-weight: normal;
    color: #7B8794;
}

QLabel#PreviewCompactChip {
    background-color: #FFFFFF;
    border: 1px solid #DCE6F0;
    border-radius: 9px;
    color: #415A77;
    font-size: 11px;
    font-weight: bold;
    padding: 4px 8px;
}

QLabel#PreviewCanvas {
    background-color: #F8F9FA;
    border: none;
    border-radius: 10px;
    color: #909399;
    font-weight: normal;
}

QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 6px 2px 6px 0;
}
QScrollBar::handle:vertical {
    background: rgba(120, 138, 156, 120);
    border-radius: 4px;
    min-height: 28px;
}
QScrollBar::handle:vertical:hover {
    background: rgba(120, 138, 156, 170);
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical,
QScrollBar::add-page:vertical,
QScrollBar::sub-page:vertical {
    background: transparent;
    border: none;
    height: 0px;
}
QScrollBar:horizontal {
    background: transparent;
    height: 8px;
    margin: 0 6px 2px 6px;
}
QScrollBar::handle:horizontal {
    background: rgba(120, 138, 156, 120);
    border-radius: 4px;
    min-width: 28px;
}
QScrollBar::handle:horizontal:hover {
    background: rgba(120, 138, 156, 170);
}
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal,
QScrollBar::add-page:horizontal,
QScrollBar::sub-page:horizontal {
    background: transparent;
    border: none;
    width: 0px;
}

QProgressBar {
    border: none;
    background-color: #E2E8F0;
    border-radius: 10px;
    text-align: center;
    color: #2D3748;
    font-weight: bold;
    font-size: 12px;
    min-height: 18px;
    max-height: 20px;
}
QProgressBar::chunk {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #48BB78, stop:1 #81E6D9);
    border-radius: 10px;
}

QSplitter::handle {
    background-color: #DDE6F1;
}

QTabWidget::pane {
    border: none;
    background: transparent;
}

QTabWidget::tab-bar {
    alignment: center;
}

QTabBar::tab {
    background-color: #E4E7EB;
    color: #7F8C8D;
    border-radius: 8px;
    padding: 10px 14px;
    margin-right: 8px;
    margin-bottom: 10px;
    font-weight: bold;
    font-size: 13px;
}

QTabBar::tab:hover {
    background-color: #D1D8E0;
    color: #2C3E50;
}

QTabBar::tab:selected {
    background-color: #4A90E2;
    color: #FFFFFF;
}

QScrollArea#LeftTabScrollArea {
    border: none;
    background: transparent;
}

QScrollArea#LeftTabScrollArea > QWidget > QWidget {
    background: transparent;
}

QPushButton#BtnAction {
    min-height: 52px;
    font-size: 15px;
}
"""

LIVE_STATE_DISPLAY: Dict[str, str] = {
    "Idle": "空闲",
    "Run": "运行中",
    "Hold": "暂停",
    "Alarm": "报警",
}

MATERIAL_PRESETS: Dict[str, Dict[str, int]] = {
    "椴木板 (切割)": {"power": 800, "feed_rate": 1500},
    "牛皮纸 (打标)": {"power": 300, "feed_rate": 3000},
    "亚克力 (深雕)": {"power": 1000, "feed_rate": 800},
}

TRANSPORT_DISPLAY: Dict[str, str] = {
    "serial": "串口 UART",
    "wifi": "WiFi TCP",
}

WIFI_WORKFLOW_PRESETS: Dict[str, Dict[str, object]] = {
    "softap": {
        "label": "WiFi SoftAP",
        "host": "192.168.43.1",
        "port": 5000,
        "summary": "SoftAP 离线直连：适合本地图导入、已有 G-Code 和纯控制演示。",
        "placeholder": "通常为 192.168.43.1",
    },
    "sta": {
        "label": "WiFi STA",
        "host": "",
        "port": 5000,
        "summary": "STA 在线工作流：适合在线 AI 生图 + WiFi 下发，请填写发射板实际分配到的 IP。",
        "placeholder": "请输入路由器/手机热点下的发射板 IP",
    },
}


def format_transport_label(mode: str) -> str:
    return TRANSPORT_DISPLAY.get(mode, mode or "--")


def format_wifi_workflow_label(workflow: str) -> str:
    preset = WIFI_WORKFLOW_PRESETS.get(workflow)
    if preset is None:
        return "WiFi TCP"
    return str(preset["label"])


class ImageLabel(QtWidgets.QLabel):
    """用于显示图片的白底预览画布。"""

    def __init__(self, placeholder: str, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self._pixmap: Optional[QtGui.QPixmap] = None
        self.setObjectName("PreviewCanvas")
        self.setMinimumSize(180, 140)
        self.setAlignment(QtCore.Qt.AlignCenter)
        self.setWordWrap(True)
        self.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        self._set_placeholder(placeholder)

    def _set_placeholder(self, text: str) -> None:
        placeholder = html.escape(text)
        self.setTextFormat(QtCore.Qt.RichText)
        self.setText(
            "<div style='text-align:center; color:#909399;'>"
            "<div style='font-size:24px; line-height:1; margin-bottom:8px;'>◫</div>"
            f"<div style='font-size:12px; font-weight:400;'>{placeholder}</div>"
            "</div>"
        )

    def set_image(self, image_path: str) -> None:
        pixmap = QtGui.QPixmap(image_path)
        if pixmap.isNull():
            self._pixmap = None
            self.setTextFormat(QtCore.Qt.PlainText)
            self.setText(f"图片加载失败\n{image_path}")
            return
        self._pixmap = pixmap
        self._refresh_pixmap()

    def set_pixmap_image(self, pixmap: QtGui.QPixmap) -> None:
        if pixmap.isNull():
            self._pixmap = None
            self.setTextFormat(QtCore.Qt.PlainText)
            self.setText("图片加载失败")
            return
        self._pixmap = QtGui.QPixmap(pixmap)
        self._refresh_pixmap()

    def clear_to_placeholder(self, text: str) -> None:
        self._pixmap = None
        self.setPixmap(QtGui.QPixmap())
        self._set_placeholder(text)

    def _refresh_pixmap(self) -> None:
        if self._pixmap is None:
            return
        scaled = self._pixmap.scaled(
            self.size() - QtCore.QSize(16, 16),
            QtCore.Qt.KeepAspectRatio,
            QtCore.Qt.SmoothTransformation,
        )
        self.setPixmap(scaled)

    def resizeEvent(self, event: QtGui.QResizeEvent) -> None:  # pragma: no cover - GUI 事件
        super().resizeEvent(event)
        self._refresh_pixmap()


class ElidedLabel(QtWidgets.QLabel):
    """单行文本标签，宽度不足时自动以省略号结尾。"""

    def __init__(self, text: str = "", parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self._full_text = ""
        self.setWordWrap(False)
        self.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.setText(text)

    def setText(self, text: str) -> None:
        self._full_text = text or ""
        self._refresh_text()

    def resizeEvent(self, event: QtGui.QResizeEvent) -> None:  # pragma: no cover - GUI 事件
        super().resizeEvent(event)
        self._refresh_text()

    def _refresh_text(self) -> None:
        width = max(1, self.contentsRect().width())
        text = self.fontMetrics().elidedText(self._full_text, QtCore.Qt.ElideRight, width)
        super().setText(text)
        super().setToolTip(self._full_text if text != self._full_text else "")


class GCodePreviewWidget(QtWidgets.QWidget):
    """轻量 G-Code 轨迹预览，区分空走和激光走线。"""

    def __init__(self, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self._lines: List[str] = []
        self._background_pixmap: Optional[QtGui.QPixmap] = None
        self.setObjectName("GCodePreviewCanvas")
        self.setMinimumHeight(170)
        self.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)

    def set_gcode_lines(self, lines: List[str]) -> None:
        self._lines = list(lines)
        self.update()

    def set_background_image(self, image_path: Optional[str]) -> None:
        if not image_path:
            self._background_pixmap = None
            self.update()
            return
        pixmap = QtGui.QPixmap(str(image_path))
        self._background_pixmap = pixmap if not pixmap.isNull() else None
        self.update()

    def clear_preview(self) -> None:
        self._lines = []
        self.update()

    @staticmethod
    def _parse_axis_words(line: str) -> Dict[str, float]:
        result: Dict[str, float] = {}
        for word in line.split():
            if len(word) < 2:
                continue
            axis = word[0].upper()
            if axis not in ("X", "Y"):
                continue
            try:
                result[axis] = float(word[1:])
            except ValueError:
                continue
        return result

    def _segments(self) -> List[Tuple[float, float, float, float, bool]]:
        segments: List[Tuple[float, float, float, float, bool]] = []
        x = 0.0
        y = 0.0
        laser_on = False
        for raw_line in self._lines:
            line = raw_line.strip()
            if not line or line.startswith(";") or line.startswith("("):
                continue
            command = line.split()[0].upper() if line.split() else ""
            if command == "M3":
                laser_on = True
                continue
            if command == "M5":
                laser_on = False
                continue
            if command not in ("G0", "G00", "G1", "G01"):
                continue

            axes = self._parse_axis_words(line)
            next_x = axes.get("X", x)
            next_y = axes.get("Y", y)
            if next_x != x or next_y != y:
                segments.append((x, y, next_x, next_y, laser_on and command in ("G1", "G01")))
            x = next_x
            y = next_y
        return segments

    def paintEvent(self, event: QtGui.QPaintEvent) -> None:  # pragma: no cover - GUI 绘制
        super().paintEvent(event)
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing, True)

        rect = self.rect().adjusted(12, 12, -12, -12)
        painter.fillRect(self.rect(), QtGui.QColor("#F8FBFE"))
        painter.setPen(QtGui.QPen(QtGui.QColor("#D8E3EF"), 1))
        painter.drawRoundedRect(QtCore.QRectF(rect), 8, 8)

        segments = self._segments()
        if not segments:
            painter.setPen(QtGui.QColor("#8A9AAD"))
            painter.drawText(rect, QtCore.Qt.AlignCenter, "等待生成 G-Code 轨迹")
            painter.end()
            return

        xs = [value for segment in segments for value in (segment[0], segment[2])]
        ys = [value for segment in segments for value in (segment[1], segment[3])]
        min_x = min(xs)
        max_x = max(xs)
        min_y = min(ys)
        max_y = max(ys)
        src_w = max(max_x - min_x, 1.0)
        src_h = max(max_y - min_y, 1.0)
        pad_x = max(src_w * 0.08, 1.0)
        pad_y = max(src_h * 0.08, 1.0)
        min_x -= pad_x
        max_x += pad_x
        min_y -= pad_y
        max_y += pad_y
        src_w = max(max_x - min_x, 1.0)
        src_h = max(max_y - min_y, 1.0)

        usable = rect.adjusted(12, 12, -12, -12)
        scale = min(usable.width() / src_w, usable.height() / src_h)
        draw_w = src_w * scale
        draw_h = src_h * scale
        origin_x = usable.left() + (usable.width() - draw_w) * 0.5
        origin_y = usable.top() + (usable.height() - draw_h) * 0.5

        preview_rect = QtCore.QRectF(origin_x, origin_y, draw_w, draw_h)
        if self._background_pixmap is not None:
            painter.setOpacity(0.62)
            painter.drawPixmap(
                preview_rect.toRect(),
                self._background_pixmap.scaled(
                    preview_rect.size().toSize(),
                    QtCore.Qt.KeepAspectRatio,
                    QtCore.Qt.SmoothTransformation,
                ),
            )
            painter.setOpacity(1.0)

        painter.setPen(QtGui.QPen(QtGui.QColor("#B9C7D8"), 1))
        painter.setBrush(QtCore.Qt.NoBrush)
        painter.drawRect(preview_rect)

        def map_point(px: float, py: float) -> QtCore.QPointF:
            return QtCore.QPointF(origin_x + (px - min_x) * scale, origin_y + draw_h - (py - min_y) * scale)

        traverse_pen = QtGui.QPen(QtGui.QColor("#A7B4C4"), 1, QtCore.Qt.DashLine)
        burn_pen = QtGui.QPen(QtGui.QColor("#1F2937"), 1.8, QtCore.Qt.SolidLine, QtCore.Qt.RoundCap)
        for x0, y0, x1, y1, burn in segments:
            painter.setPen(burn_pen if burn else traverse_pen)
            painter.drawLine(map_point(x0, y0), map_point(x1, y1))

        painter.setPen(QtGui.QColor("#667085"))
        painter.drawText(rect.adjusted(8, 6, -8, -6), QtCore.Qt.AlignLeft | QtCore.Qt.AlignTop, "实线=雕刻  虚线=空走")
        painter.end()


def _draw_chevron(
    painter: QtGui.QPainter,
    rect: QtCore.QRect,
    direction: str,
    color: QtGui.QColor,
    offset_y: int = 0,
) -> None:
    if not rect.isValid() or rect.width() < 10 or rect.height() < 10:
        return

    rect_f = QtCore.QRectF(rect).translated(0.0, float(offset_y))
    center = rect_f.center()
    icon_width = min(rect_f.width() * 0.46, 13.0)
    icon_height = min(rect_f.height() * 0.24, 5.6)
    half_width = max(4.8, icon_width / 2.0)
    half_height = max(2.2, icon_height / 2.0)
    top = center.y() - half_height
    bottom = center.y() + half_height
    left = center.x() - half_width
    right = center.x() + half_width

    if direction == "up":
        points = [
            QtCore.QPointF(left, bottom),
            QtCore.QPointF(center.x(), top),
            QtCore.QPointF(right, bottom),
        ]
    else:
        points = [
            QtCore.QPointF(left, top),
            QtCore.QPointF(center.x(), bottom),
            QtCore.QPointF(right, top),
        ]

    pen = QtGui.QPen(color, 1.7, QtCore.Qt.SolidLine, QtCore.Qt.RoundCap, QtCore.Qt.RoundJoin)
    painter.setPen(pen)
    painter.setBrush(QtCore.Qt.NoBrush)
    painter.drawPolyline(QtGui.QPolygonF(points))


def _paint_spinbox_chevrons(widget: QtWidgets.QAbstractSpinBox) -> None:
    option = QtWidgets.QStyleOptionSpinBox()
    widget.initStyleOption(option)
    style = widget.style()
    up_rect = style.subControlRect(QtWidgets.QStyle.CC_SpinBox, option, QtWidgets.QStyle.SC_SpinBoxUp, widget)
    down_rect = style.subControlRect(QtWidgets.QStyle.CC_SpinBox, option, QtWidgets.QStyle.SC_SpinBoxDown, widget)

    color = widget.palette().color(QtGui.QPalette.Text)
    if not color.isValid():
        color = QtGui.QColor("#555555")
    if not widget.isEnabled():
        color = widget.palette().color(QtGui.QPalette.Disabled, QtGui.QPalette.Text)
        if not color.isValid():
            color = QtGui.QColor("#A0AEC0")
    painter = QtGui.QPainter(widget)
    painter.setRenderHint(QtGui.QPainter.Antialiasing, True)
    painter.setRenderHint(QtGui.QPainter.TextAntialiasing, True)
    _draw_chevron(painter, up_rect, "up", color)
    _draw_chevron(painter, down_rect, "down", color)
    painter.end()


def _paint_combo_chevron(widget: QtWidgets.QComboBox) -> None:
    option = QtWidgets.QStyleOptionComboBox()
    widget.initStyleOption(option)
    style = widget.style()
    drop_rect = style.subControlRect(QtWidgets.QStyle.CC_ComboBox, option, QtWidgets.QStyle.SC_ComboBoxArrow, widget)

    color = widget.palette().color(QtGui.QPalette.Text)
    if not color.isValid():
        color = QtGui.QColor("#555555")
    if not widget.isEnabled():
        color = widget.palette().color(QtGui.QPalette.Disabled, QtGui.QPalette.Text)
        if not color.isValid():
            color = QtGui.QColor("#A0AEC0")
    painter = QtGui.QPainter(widget)
    painter.setRenderHint(QtGui.QPainter.Antialiasing, True)
    painter.setRenderHint(QtGui.QPainter.TextAntialiasing, True)
    _draw_chevron(painter, drop_rect, "down", color)
    painter.end()


class ClickToEditSpinBox(QtWidgets.QSpinBox):
    """
    更适合课堂演示的参数输入框。

    设计目标：
    - 只有用户左键点中后，才允许通过滚轮或键盘修改；
    - 避免鼠标滑过参数区时误触发数值变化。
    """

    def __init__(self, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self.setFocusPolicy(QtCore.Qt.ClickFocus)
        self.setKeyboardTracking(False)

    def wheelEvent(self, event: QtGui.QWheelEvent) -> None:  # pragma: no cover - GUI 事件
        if self.hasFocus():
            super().wheelEvent(event)
            return
        event.ignore()

    def mousePressEvent(self, event: QtGui.QMouseEvent) -> None:  # pragma: no cover - GUI 事件
        if event.button() == QtCore.Qt.LeftButton:
            self.setFocus(QtCore.Qt.MouseFocusReason)
        super().mousePressEvent(event)

    def paintEvent(self, event: QtGui.QPaintEvent) -> None:  # pragma: no cover - GUI 事件
        super().paintEvent(event)
        _paint_spinbox_chevrons(self)


class ClickToEditDoubleSpinBox(QtWidgets.QDoubleSpinBox):
    """双精度版本，行为与 ClickToEditSpinBox 保持一致。"""

    def __init__(self, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self.setFocusPolicy(QtCore.Qt.ClickFocus)
        self.setKeyboardTracking(False)

    def wheelEvent(self, event: QtGui.QWheelEvent) -> None:  # pragma: no cover - GUI 事件
        if self.hasFocus():
            super().wheelEvent(event)
            return
        event.ignore()

    def mousePressEvent(self, event: QtGui.QMouseEvent) -> None:  # pragma: no cover - GUI 事件
        if event.button() == QtCore.Qt.LeftButton:
            self.setFocus(QtCore.Qt.MouseFocusReason)
        super().mousePressEvent(event)

    def paintEvent(self, event: QtGui.QPaintEvent) -> None:  # pragma: no cover - GUI 事件
        super().paintEvent(event)
        _paint_spinbox_chevrons(self)


class ClickToSelectComboBox(QtWidgets.QComboBox):
    """
    默认忽略滚轮，只有展开下拉列表后才允许滚轮切换。

    设计目标：
    - 防止鼠标经过时误改连接方式、串口或波特率；
    - 用户必须先点击控件，明确进入选择动作。
    """

    def __init__(self, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self.setFocusPolicy(QtCore.Qt.ClickFocus)

    def wheelEvent(self, event: QtGui.QWheelEvent) -> None:  # pragma: no cover - GUI 事件
        if self.view().isVisible():
            super().wheelEvent(event)
            return
        event.ignore()

    def paintEvent(self, event: QtGui.QPaintEvent) -> None:  # pragma: no cover - GUI 事件
        super().paintEvent(event)
        _paint_combo_chevron(self)


class EditableBaudComboBox(ClickToSelectComboBox):
    """可手动输入波特率，同时保留常见预设值。"""

    def __init__(self, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self.setEditable(True)
        self.setInsertPolicy(QtWidgets.QComboBox.NoInsert)
        self.setDuplicatesEnabled(False)
        self.addItems(["115200", "230400", "460800", "921600"])
        self.setCurrentText("115200")

        line_edit = self.lineEdit()
        if line_edit is not None:
            line_edit.setPlaceholderText("输入自定义波特率")
            line_edit.setValidator(QtGui.QIntValidator(1, 10000000, line_edit))


class HoverCardFrame(QtWidgets.QFrame):
    """带悬浮进入/离开信号的卡片容器，用于极简浮层操作按钮。"""

    hover_changed = QtCore.Signal(bool)
    clicked = QtCore.Signal()

    def enterEvent(self, event: QtCore.QEvent) -> None:  # pragma: no cover - GUI 事件
        self.hover_changed.emit(True)
        super().enterEvent(event)

    def leaveEvent(self, event: QtCore.QEvent) -> None:  # pragma: no cover - GUI 事件
        self.hover_changed.emit(False)
        super().leaveEvent(event)

    def mousePressEvent(self, event: QtGui.QMouseEvent) -> None:  # pragma: no cover - GUI 事件
        if event.button() == QtCore.Qt.LeftButton:
            self.clicked.emit()
        super().mousePressEvent(event)


class ChevronToolButton(QtWidgets.QToolButton):
    """绘制细线 chevron 图标，替代文本字符折叠箭头。"""

    def __init__(self, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self.setCursor(QtCore.Qt.PointingHandCursor)
        self.setText("")

    def paintEvent(self, event: QtGui.QPaintEvent) -> None:  # pragma: no cover - GUI 事件
        super().paintEvent(event)
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing, True)

        color = self.palette().buttonText().color()
        pen = QtGui.QPen(color, 1.7, QtCore.Qt.SolidLine, QtCore.Qt.RoundCap, QtCore.Qt.RoundJoin)
        painter.setPen(pen)

        rect = self.rect().adjusted(6, 7, -6, -7)
        center_x = rect.center().x()
        if self.isChecked():
            points = [
                QtCore.QPointF(rect.left(), rect.top()),
                QtCore.QPointF(center_x, rect.bottom()),
                QtCore.QPointF(rect.right(), rect.top()),
            ]
        else:
            points = [
                QtCore.QPointF(rect.left(), rect.bottom()),
                QtCore.QPointF(center_x, rect.top()),
                QtCore.QPointF(rect.right(), rect.bottom()),
            ]
        painter.drawPolyline(QtGui.QPolygonF(points))


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(f"✨ AI 智能创作中枢 - {QT_API}")
        self.resize(1440, 900)
        self.project_root = Path(__file__).resolve().parents[1]
        self.history_root = self.project_root / "history"
        self.history_root.mkdir(parents=True, exist_ok=True)

        self.current_image_path: Optional[str] = None
        self.current_contours_path: Optional[str] = None
        self.current_contours: List[NormalizedPath] = []
        self.current_gcode_lines: List[str] = []
        self.current_preview_lines: List[str] = []
        self.current_history_dir: Optional[Path] = None
        self.current_source_type = "local"
        self.ai_thread: Optional[AIGeneratorThread] = None
        self.prompt_optimizer_thread: Optional[PromptOptimizerThread] = None
        self._pending_auto_generate = False
        self._pending_auto_send = False
        self._is_generating_gcode = False
        self._is_prompt_optimizing = False
        self._active_device_job = ""
        self._current_wifi_workflow = "softap"
        self.task_manager = TaskManager()
        self.task_manager.add_state_listener(self._on_task_state_changed)

        self.serial_thread = SerialWorkerThread(self)
        self.serial_thread.log_signal.connect(self.append_log)
        self.serial_thread.status_signal.connect(self.append_log)
        self.serial_thread.progress_signal.connect(self.on_progress_changed)
        self.serial_thread.job_progress_signal.connect(self.on_job_line_progress)
        self.serial_thread.ports_signal.connect(self.update_ports)
        self.serial_thread.telemetry_signal.connect(self.on_live_status_changed)
        self.serial_thread.finished_signal.connect(self.on_job_finished)
        self.serial_thread.start()

        self._build_ui()
        self.apply_stylesheet()
        self._update_connection_ui(False)
        self.reset_live_status_strip()
        self._refresh_task_status_panel()
        QtCore.QTimer.singleShot(0, self._position_preview_floating_bar)
        self._refresh_history_gallery()
        self.refresh_ports()
        self.append_log("欢迎使用 AI 智能创作中枢，准备开始创作吧。")
        self.append_log(
            f"当前板卡工作区: X 0~{BOARD_WORK_AREA_X_MM:.1f} mm / Y 0~{BOARD_WORK_AREA_Y_MM:.1f} mm，"
            "原点按 LaserGRBL 习惯位于工作区左下角。"
        )

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # pragma: no cover - GUI 事件
        self.task_manager.remove_state_listener(self._on_task_state_changed)
        self.serial_thread.stop()
        super().closeEvent(event)

    def resizeEvent(self, event: QtGui.QResizeEvent) -> None:  # pragma: no cover - GUI 事件
        super().resizeEvent(event)
        self._position_preview_floating_bar()

    def _build_ui(self) -> None:
        central = QtWidgets.QWidget()
        self.setCentralWidget(central)

        root_layout = QtWidgets.QHBoxLayout(central)
        root_layout.setContentsMargins(12, 12, 12, 12)
        root_layout.setSpacing(0)

        self.horizontal_splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        self.horizontal_splitter.setChildrenCollapsible(False)
        self.horizontal_splitter.setHandleWidth(8)
        self.horizontal_splitter.setOpaqueResize(True)
        root_layout.addWidget(self.horizontal_splitter)

        left_panel = self._build_left_panel()
        right_panel = self._build_right_panel()

        self.horizontal_splitter.addWidget(left_panel)
        self.horizontal_splitter.addWidget(right_panel)
        self.horizontal_splitter.setStretchFactor(0, 1)
        self.horizontal_splitter.setStretchFactor(1, 3)
        self.horizontal_splitter.setSizes([410, 1030])

    def _build_left_panel(self) -> QtWidgets.QWidget:
        self.left_panel = QtWidgets.QWidget()
        self.left_panel.setMinimumWidth(390)
        self.left_panel.setMaximumWidth(460)
        self.left_panel.setSizePolicy(QtWidgets.QSizePolicy.Preferred, QtWidgets.QSizePolicy.Expanding)

        self.left_layout = QtWidgets.QVBoxLayout(self.left_panel)
        self.left_layout.setContentsMargins(0, 0, 0, 0)
        self.left_layout.setSpacing(10)

        title = QtWidgets.QLabel("✨ AI 智能创作中枢")
        title.setStyleSheet("font-size:22px; font-weight:800; color:#2C3E50; padding: 6px 12px;")
        self.left_layout.addWidget(title)

        subtitle = QtWidgets.QLabel("让学生从创意出发，一步完成图像生成、轮廓提取与激光雕刻。")
        subtitle.setWordWrap(True)
        subtitle.setStyleSheet("font-size:13px; color:#7F8C8D; font-weight:normal; padding: 0 12px 8px 12px;")
        self.left_layout.addWidget(subtitle)

        self.left_layout.addWidget(self._build_left_tabs(), 1)
        return self.left_panel

    def _create_left_tab_page(self, *widgets: QtWidgets.QWidget) -> QtWidgets.QWidget:
        page = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)
        last_index = len(widgets) - 1
        for index, widget in enumerate(widgets):
            layout.addWidget(widget)
            if index == last_index:
                layout.setStretch(index, 1)
        return page

    def _build_left_tabs(self) -> QtWidgets.QTabWidget:
        self.left_tabs = QtWidgets.QTabWidget()
        self.left_tabs.setDocumentMode(True)
        self.left_tabs.setTabPosition(QtWidgets.QTabWidget.North)
        self.left_tabs.setUsesScrollButtons(False)
        self.left_tabs.tabBar().setElideMode(QtCore.Qt.ElideNone)
        self.left_tabs.tabBar().setExpanding(True)
        self.left_tabs.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)

        serial_page = self._build_left_scroll_page(
            self._build_connection_card(),
            self._build_left_fill_panel(),
        )
        ai_page = self._build_left_scroll_page(self._build_ai_card())
        self.machine_page = self._build_machine_scroll_page()
        self.history_page = self._build_history_page()

        self.left_tabs.addTab(serial_page, "连接")
        self.left_tabs.addTab(ai_page, "创作")
        self.left_tabs.addTab(self.machine_page, "雕刻")
        self.left_tabs.addTab(self.history_page, "历史")
        self.left_tabs.setTabToolTip(0, "步骤一：连接设备")
        self.left_tabs.setTabToolTip(1, "步骤二：AI 创作与导入")
        self.left_tabs.setTabToolTip(2, "步骤三：雕刻执行")
        self.left_tabs.setTabToolTip(3, "历史任务与本地缓存")
        return self.left_tabs

    def _build_left_scroll_page(self, *widgets: QtWidgets.QWidget) -> QtWidgets.QScrollArea:
        """为左侧高内容页提供统一的纵向滚动能力，避免控件被压缩。"""

        scroll = QtWidgets.QScrollArea()
        scroll.setObjectName("LeftTabScrollArea")
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        scroll.setVerticalScrollBarPolicy(QtCore.Qt.ScrollBarAsNeeded)
        scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

        container = QtWidgets.QWidget()
        container_layout = QtWidgets.QVBoxLayout(container)
        container_layout.setContentsMargins(0, 0, 0, 0)
        container_layout.setSpacing(8)
        for widget in widgets:
            container_layout.addWidget(widget)
        container_layout.addStretch(1)

        scroll.setWidget(container)
        return scroll

    def _build_machine_scroll_page(self) -> QtWidgets.QScrollArea:
        """步骤三内容较高，单独放入滚动区，避免卡片和按钮被挤压。"""

        return self._build_left_scroll_page(
            self._build_params_card(),
            self._build_contour_params_card(),
            self._build_action_card(),
        )

    def _build_history_page(self) -> QtWidgets.QScrollArea:
        scroll = QtWidgets.QScrollArea()
        scroll.setObjectName("LeftTabScrollArea")
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        scroll.setVerticalScrollBarPolicy(QtCore.Qt.ScrollBarAsNeeded)
        scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

        container = QtWidgets.QWidget()
        self.history_list_layout = QtWidgets.QVBoxLayout(container)
        self.history_list_layout.setContentsMargins(0, 0, 0, 0)
        self.history_list_layout.setSpacing(10)
        scroll.setWidget(container)
        return scroll

    def _build_right_panel(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(panel)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)

        # -- 状态条：固定高度，不参与 splitter 拉伸 --
        status_card = self._build_status_card()
        layout.addWidget(status_card)

        # -- 预览 + 日志：通过 splitter 共享剩余空间 --
        self.right_splitter = QtWidgets.QSplitter(QtCore.Qt.Vertical)
        preview_card = self._build_preview_card()
        log_card = self._build_log_card()

        log_card.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        self.right_splitter.addWidget(preview_card)
        self.right_splitter.addWidget(log_card)
        self.right_splitter.setChildrenCollapsible(False)
        self.right_splitter.setHandleWidth(8)
        self.right_splitter.setOpaqueResize(True)
        self.right_splitter.setStretchFactor(0, 3)
        self.right_splitter.setStretchFactor(1, 1)
        self.right_splitter.setSizes([560, 240])

        layout.addWidget(self.right_splitter, 1)
        return panel

    def _build_status_card(self) -> QtWidgets.QFrame:
        card = QtWidgets.QFrame()
        card.setObjectName("StatusCard")
        card.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)

        layout = QtWidgets.QVBoxLayout(card)
        layout.setContentsMargins(12, 8, 12, 8)
        layout.setSpacing(4)

        # -- Row 1: title + progress bar --
        header_row = QtWidgets.QHBoxLayout()
        header_row.setSpacing(10)

        title_label = QtWidgets.QLabel("\U0001f4e1 \u72b6\u6001\u4e0e\u8fdb\u5ea6")
        title_label.setObjectName("CardTitle")
        title_label.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Preferred)
        header_row.addWidget(title_label)

        self.task_status_label = QtWidgets.QLabel()
        self.task_status_label.setObjectName("TaskStatusLabel")
        self.task_status_label.setAlignment(QtCore.Qt.AlignVCenter | QtCore.Qt.AlignLeft)
        self.task_status_label.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Preferred)
        header_row.addWidget(self.task_status_label)

        self.progress_bar = QtWidgets.QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setFormat("\u4efb\u52a1\u8fdb\u5ea6 %p%")
        self.progress_bar.setFixedHeight(20)
        self.progress_bar.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        header_row.addWidget(self.progress_bar, 1)

        layout.addLayout(header_row)

        self.task_detail_label = QtWidgets.QLabel()
        self.task_detail_label.setObjectName("TaskDetailLabel")
        self.task_detail_label.setWordWrap(True)
        self.task_detail_label.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Preferred)
        self.task_detail_label.setMaximumHeight(32)
        layout.addWidget(self.task_detail_label)

        # -- Row 2-3: 紧凑状态栏 --
        telemetry_strip = QtWidgets.QFrame()
        telemetry_strip.setObjectName("LiveTelemetryStrip")
        telemetry_strip.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        telemetry_layout = QtWidgets.QVBoxLayout(telemetry_strip)
        telemetry_layout.setContentsMargins(8, 6, 8, 6)
        telemetry_layout.setSpacing(2)

        network_row = QtWidgets.QHBoxLayout()
        network_row.setContentsMargins(0, 0, 0, 0)
        network_row.setSpacing(4)
        self.live_state_value = self._append_live_status_field(network_row, "设备")
        self._append_status_separator(network_row)
        self.live_transport_value = self._append_live_status_field(network_row, "链路")
        self._append_status_separator(network_row)
        self.live_wifi_mode_value = self._append_live_status_field(network_row, "WiFi")
        self._append_status_separator(network_row)
        self.live_sle_value = self._append_live_status_field(network_row, "SLE")
        self._append_status_separator(network_row)
        self.live_refresh_value = self._append_live_status_field(network_row, "刷新")
        network_row.addStretch(1)
        telemetry_layout.addLayout(network_row)

        telemetry_row = QtWidgets.QHBoxLayout()
        telemetry_row.setContentsMargins(0, 0, 0, 0)
        telemetry_row.setSpacing(4)
        self.live_coord_value = self._append_live_status_field(telemetry_row, "坐标(X,Y)")
        self._append_status_separator(telemetry_row)
        self.live_target_value = self._append_live_status_field(telemetry_row, "目标", stretch=2, elide=True)
        self._append_status_separator(telemetry_row)
        self.live_feed_value = self._append_live_status_field(telemetry_row, "速度(F)")
        self._append_status_separator(telemetry_row)
        self.live_power_value = self._append_live_status_field(telemetry_row, "功率(S)")
        telemetry_row.addStretch(1)
        telemetry_layout.addLayout(telemetry_row)

        layout.addWidget(telemetry_strip)
        return card

    def _create_card(self, title: str, hint: str = "") -> Tuple[QtWidgets.QFrame, QtWidgets.QVBoxLayout]:
        card = QtWidgets.QFrame()
        card.setObjectName("Card")
        card.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Maximum)
        card_layout = QtWidgets.QVBoxLayout(card)
        card_layout.setContentsMargins(16, 14, 16, 14)
        card_layout.setSpacing(10)

        title_label = QtWidgets.QLabel(title)
        title_label.setObjectName("CardTitle")
        card_layout.addWidget(title_label)

        if hint:
            hint_label = QtWidgets.QLabel(hint)
            hint_label.setObjectName("CardHint")
            hint_label.setWordWrap(True)
            card_layout.addWidget(hint_label)

        return card, card_layout

    def _apply_min_height(self, *widgets: QtWidgets.QWidget, height: int = 32) -> None:
        for widget in widgets:
            widget.setMinimumHeight(height)

    def _build_connection_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("🔌 设备连接", "比赛版支持实物串口 UART 与 WiFi TCP 两种设备接入方式。")

        self.transport_combo = ClickToSelectComboBox()
        self.transport_combo.addItem("串口 UART", "serial")
        self.transport_combo.addItem("WiFi TCP", "wifi")
        self.transport_combo.currentIndexChanged.connect(self.on_transport_mode_changed)
        self._apply_min_height(self.transport_combo)
        layout.addWidget(QtWidgets.QLabel("连接方式"))
        layout.addWidget(self.transport_combo)

        preset_row = QtWidgets.QHBoxLayout()
        preset_row.setContentsMargins(0, 0, 0, 0)
        preset_row.setSpacing(8)
        self.serial_preset_button = QtWidgets.QPushButton("串口模式")
        self.serial_preset_button.setProperty("secondary", True)
        self.serial_preset_button.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.serial_preset_button.clicked.connect(lambda: self._apply_connection_preset("serial"))
        self.wifi_softap_button = QtWidgets.QPushButton("WiFi SoftAP")
        self.wifi_softap_button.setProperty("secondary", True)
        self.wifi_softap_button.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.wifi_softap_button.clicked.connect(lambda: self._apply_connection_preset("softap"))
        self.wifi_sta_button = QtWidgets.QPushButton("WiFi STA")
        self.wifi_sta_button.setProperty("secondary", True)
        self.wifi_sta_button.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.wifi_sta_button.clicked.connect(lambda: self._apply_connection_preset("sta"))
        self._apply_min_height(self.serial_preset_button, self.wifi_softap_button, self.wifi_sta_button)
        preset_row.addWidget(self.serial_preset_button)
        preset_row.addWidget(self.wifi_softap_button)
        preset_row.addWidget(self.wifi_sta_button)
        layout.addWidget(QtWidgets.QLabel("快捷预设"))
        layout.addLayout(preset_row)

        self.port_combo = ClickToSelectComboBox()
        self.port_combo.setSizeAdjustPolicy(
            QtWidgets.QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon
        )
        self.port_combo.setMinimumContentsLength(10)
        self.port_combo.setSizePolicy(QtWidgets.QSizePolicy.MinimumExpanding, QtWidgets.QSizePolicy.Fixed)
        self.port_combo.setMinimumWidth(0)
        self.refresh_ports_button = QtWidgets.QPushButton("刷新")
        self.refresh_ports_button.setSizePolicy(QtWidgets.QSizePolicy.Fixed, QtWidgets.QSizePolicy.Fixed)
        self.refresh_ports_button.clicked.connect(self.refresh_ports)
        self._apply_min_height(self.port_combo, self.refresh_ports_button)

        self.baud_combo = EditableBaudComboBox()
        self._apply_min_height(self.baud_combo)

        self.serial_config_widget = QtWidgets.QWidget()
        serial_layout = QtWidgets.QVBoxLayout(self.serial_config_widget)
        serial_layout.setContentsMargins(0, 0, 0, 0)
        serial_layout.setSpacing(8)
        port_label = QtWidgets.QLabel("可用串口")
        serial_layout.addWidget(port_label)
        port_row = QtWidgets.QHBoxLayout()
        port_row.setSpacing(8)
        port_row.setContentsMargins(0, 0, 0, 0)
        port_row.addWidget(self.port_combo, 1)
        port_row.addWidget(self.refresh_ports_button, 0)
        serial_layout.addLayout(port_row)
        serial_layout.addWidget(QtWidgets.QLabel("波特率"))
        serial_layout.addWidget(self.baud_combo)
        layout.addWidget(self.serial_config_widget)

        self.wifi_host_edit = QtWidgets.QLineEdit("192.168.43.1")
        self.wifi_host_edit.setPlaceholderText("通常为 192.168.43.1")
        self.wifi_port_spin = ClickToEditSpinBox()
        self.wifi_port_spin.setRange(1, 65535)
        self.wifi_port_spin.setValue(5000)
        self._apply_min_height(self.wifi_host_edit, self.wifi_port_spin)

        self.wifi_config_widget = QtWidgets.QWidget()
        wifi_layout = QtWidgets.QFormLayout(self.wifi_config_widget)
        wifi_layout.setContentsMargins(0, 0, 0, 0)
        wifi_layout.setHorizontalSpacing(12)
        wifi_layout.setVerticalSpacing(8)
        wifi_layout.addRow("设备 IP", self.wifi_host_edit)
        wifi_layout.addRow("TCP 端口", self.wifi_port_spin)
        layout.addWidget(self.wifi_config_widget)

        self.connect_button = QtWidgets.QPushButton("连接设备")
        self.connect_button.setObjectName("BtnPrimary")
        self.connect_button.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.connect_button.clicked.connect(self.toggle_device_connection)
        self._apply_min_height(self.connect_button)
        layout.addWidget(self.connect_button)

        self.connection_summary_label = QtWidgets.QLabel("当前未连接设备。")
        self.connection_summary_label.setWordWrap(True)
        self.connection_summary_label.setStyleSheet("font-size:12px; font-weight:normal; color:#718096;")
        layout.addWidget(self.connection_summary_label)

        self.on_transport_mode_changed()
        return card

    def _build_left_fill_panel(self) -> QtWidgets.QFrame:
        panel = QtWidgets.QFrame()
        panel.setObjectName("LeftFillPanel")
        panel.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)

        layout = QtWidgets.QVBoxLayout(panel)
        layout.setContentsMargins(18, 18, 18, 18)
        layout.setSpacing(10)

        title = QtWidgets.QLabel("连接就绪提示")
        title.setObjectName("LeftFillTitle")
        layout.addWidget(title)

        summary = QtWidgets.QLabel(
            "这一页主要负责设备接入。连接成功后，右侧会实时刷新坐标、功率、速度和链路状态。"
        )
        summary.setObjectName("LeftFillText")
        summary.setWordWrap(True)
        layout.addWidget(summary)

        points = [
            "UART 适合基础联调，链路更直接。",
            "WiFi TCP 适合无线演示，可配合 SoftAP 或 STA。",
            "如现场网络不稳定，建议优先走本地图导入流程。",
        ]
        for text in points:
            label = QtWidgets.QLabel(f"• {text}")
            label.setObjectName("LeftFillText")
            label.setWordWrap(True)
            layout.addWidget(label)

        layout.addStretch(1)
        footer = QtWidgets.QLabel("连接完成后，可切到“创作”或“雕刻”继续执行。")
        footer.setObjectName("LeftFillText")
        footer.setWordWrap(True)
        layout.addWidget(footer)
        return panel

    def _build_ai_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("🎨 AI 创作区", "输入提示词生成图像，或直接导入本地图片进行雕刻。")

        layout.addWidget(QtWidgets.QLabel("生图模型"))
        self.image_model_combo = ClickToSelectComboBox()
        for model_name in ("通义万相 2.7", "豆包 Seedream 5.0"):
            self.image_model_combo.addItem(model_name, model_name)
        self.image_model_combo.setCurrentText(DEFAULT_MODEL_LABEL)
        self._apply_min_height(self.image_model_combo)
        layout.addWidget(self.image_model_combo)

        layout.addWidget(QtWidgets.QLabel("创意描述"))
        self.prompt_text_edit = QtWidgets.QTextEdit()
        self.prompt_text_edit.setObjectName("PromptBox")
        self.prompt_text_edit.setPlaceholderText("例如：画一只戴着护目镜的创客小猫，线稿风格，适合激光雕刻")
        self.prompt_text_edit.setMinimumHeight(120)
        self.prompt_text_edit.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.prompt_edit = self.prompt_text_edit
        layout.addWidget(self.prompt_text_edit)

        self.btn_beautify_prompt = QtWidgets.QPushButton("✨ 一键美化提示词")
        self.btn_beautify_prompt.setObjectName("BtnPromptAssist")
        self.btn_beautify_prompt.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.btn_beautify_prompt.clicked.connect(self.on_beautify_prompt)
        self._apply_min_height(self.btn_beautify_prompt)
        layout.addWidget(self.btn_beautify_prompt)

        self.generate_image_button = QtWidgets.QPushButton("✨ 魔法生成图像")
        self.generate_image_button.setObjectName("BtnAI")
        self.generate_image_button.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.generate_image_button.clicked.connect(self.on_generate_image)
        self._apply_min_height(self.generate_image_button)
        layout.addWidget(self.generate_image_button)

        self.competition_flow_button = QtWidgets.QPushButton("⚡ 一键比赛流程")
        self.competition_flow_button.setObjectName("BtnStart")
        self.competition_flow_button.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.competition_flow_button.clicked.connect(self.on_start_competition_flow)
        self._apply_min_height(self.competition_flow_button)
        layout.addWidget(self.competition_flow_button)

        self.import_image_button = QtWidgets.QPushButton("📁 导入本地图片")
        self.import_image_button.setObjectName("BtnLocal")
        self.import_image_button.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.import_image_button.clicked.connect(self.on_import_local_image)
        self._apply_min_height(self.import_image_button)
        layout.addWidget(self.import_image_button)

        self.competition_hint_label = QtWidgets.QLabel(
            "比赛流程会自动执行：AI 生图 -> 轮廓提取 -> G-Code 生成 -> 发送到当前设备。"
        )
        self.competition_hint_label.setWordWrap(True)
        self.competition_hint_label.setStyleSheet("font-size:12px; font-weight:normal; color:#718096;")
        layout.addWidget(self.competition_hint_label)

        return card

    def _build_params_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card(
            "⚙️ 雕刻参数",
            (
                f"当前板子采用 LaserGRBL 常见的第一象限工作方式。"
                f"有效幅面为 X 0~{BOARD_WORK_AREA_X_MM:.1f} mm / Y 0~{BOARD_WORK_AREA_Y_MM:.1f} mm。"
            ),
        )

        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignLeft)
        form.setFormAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignTop)
        form.setHorizontalSpacing(12)
        form.setVerticalSpacing(10)

        self.width_spin = ClickToEditDoubleSpinBox()
        self.width_spin.setRange(1.0, BOARD_WORK_AREA_X_MM)
        self.width_spin.setValue(50.0)
        self.width_spin.setSuffix(" mm")
        self.width_spin.setSingleStep(1.0)

        self.height_spin = ClickToEditDoubleSpinBox()
        self.height_spin.setRange(1.0, BOARD_WORK_AREA_Y_MM)
        self.height_spin.setValue(50.0)
        self.height_spin.setSuffix(" mm")
        self.height_spin.setSingleStep(1.0)

        self.material_preset_combo = ClickToSelectComboBox()
        self.material_preset_combo.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.material_preset_combo.addItem("手动设置", "")
        for preset_name in MATERIAL_PRESETS:
            self.material_preset_combo.addItem(preset_name, preset_name)
        self.material_preset_combo.currentIndexChanged.connect(lambda _index=0: self.on_material_preset_changed())

        self.feed_spin = ClickToEditSpinBox()
        self.feed_spin.setRange(1, 100000)
        self.feed_spin.setValue(6000)

        self.power_spin = ClickToEditSpinBox()
        self.power_spin.setRange(0, 1000)
        self.power_spin.setValue(200)

        self._apply_min_height(
            self.width_spin,
            self.height_spin,
            self.material_preset_combo,
            self.feed_spin,
            self.power_spin,
        )

        form.addRow("宽度", self.width_spin)
        form.addRow("高度", self.height_spin)
        form.addRow("材质预设", self.material_preset_combo)
        form.addRow("进给速度 F", self.feed_spin)
        form.addRow("激光功率 S", self.power_spin)
        layout.addLayout(form)

        board_hint = QtWidgets.QLabel(
            f"坐标说明：G-Code 输出保持正坐标，`G0 X0 Y0` 表示工作区原点，不是振镜中心。"
        )
        board_hint.setWordWrap(True)
        board_hint.setStyleSheet("font-size:12px; font-weight:normal; color:#718096;")
        layout.addWidget(board_hint)
        return card

    def _build_contour_params_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("✏️ 轮廓提取参数", "图片已经导入或生成后，可调整参数并重新提取轮廓。")

        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignLeft)
        form.setFormAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignTop)
        form.setHorizontalSpacing(12)
        form.setVerticalSpacing(10)

        self.contour_detail_spin = ClickToEditSpinBox()
        self.contour_detail_spin.setRange(1, 5)
        self.contour_detail_spin.setValue(3)

        self.contour_denoise_spin = ClickToEditSpinBox()
        self.contour_denoise_spin.setRange(0, 5)
        self.contour_denoise_spin.setValue(2)

        self.contour_min_area_spin = ClickToEditSpinBox()
        self.contour_min_area_spin.setRange(1, 1000)
        self.contour_min_area_spin.setValue(20)

        self.contour_smooth_spin = ClickToEditSpinBox()
        self.contour_smooth_spin.setRange(1, 5)
        self.contour_smooth_spin.setValue(2)

        self._apply_min_height(
            self.contour_detail_spin,
            self.contour_denoise_spin,
            self.contour_min_area_spin,
            self.contour_smooth_spin,
        )

        form.addRow("细节保留", self.contour_detail_spin)
        form.addRow("降噪强度", self.contour_denoise_spin)
        form.addRow("最小面积", self.contour_min_area_spin)
        form.addRow("线条平滑", self.contour_smooth_spin)
        layout.addLayout(form)

        self.reprocess_contour_button = QtWidgets.QPushButton("重新提取轮廓")
        self.reprocess_contour_button.setObjectName("BtnAction")
        self.reprocess_contour_button.clicked.connect(self.on_reprocess_current_image)
        self._apply_min_height(self.reprocess_contour_button)
        layout.addWidget(self.reprocess_contour_button)
        return card

    def _build_action_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("🕹️ 操作控制", "生成 G-Code、预览边界、开始雕刻或在紧急情况下立即停止。")

        self.generate_gcode_button = QtWidgets.QPushButton("生成 G-Code")
        self.generate_gcode_button.setObjectName("BtnAction")
        self.generate_gcode_button.clicked.connect(self.on_generate_gcode)

        self.preview_button = QtWidgets.QPushButton("🎯 红光边框预览")
        self.preview_button.setObjectName("BtnAction")
        self.preview_button.clicked.connect(self.on_preview_bbox)

        self.start_button = QtWidgets.QPushButton("🚀 开始激光雕刻")
        self.start_button.setObjectName("BtnStart")
        self.start_button.clicked.connect(self.on_start_engraving)

        self.export_button = QtWidgets.QPushButton("导出 G-Code 文件")
        self.export_button.setObjectName("BtnAction")
        self.export_button.clicked.connect(self.on_export_gcode)

        self.emergency_button = QtWidgets.QPushButton("🛑 紧急停止")
        self.emergency_button.setObjectName("BtnStop")
        self.emergency_button.clicked.connect(self.on_emergency_stop)

        self._apply_min_height(
            self.generate_gcode_button,
            self.preview_button,
            self.start_button,
            self.export_button,
            self.emergency_button,
            height=52,
        )

        for button in (
            self.generate_gcode_button,
            self.preview_button,
            self.start_button,
            self.export_button,
            self.emergency_button,
        ):
            button.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
            layout.addWidget(button)
        return card

    def _build_preview_card(self) -> QtWidgets.QFrame:
        card = HoverCardFrame()
        card.setObjectName("Card")
        card.setProperty("previewRole", "floating")
        card.setProperty("previewCollapsed", False)
        card.setProperty("previewHover", False)
        card.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Preferred)
        card.setMinimumHeight(240)
        card.hover_changed.connect(self._set_preview_toolbar_hovered)
        card.clicked.connect(self._on_preview_card_clicked)
        self.preview_card = card

        layout = QtWidgets.QVBoxLayout(card)
        self.preview_card_layout = layout
        layout.setContentsMargins(16, 14, 16, 14)
        layout.setSpacing(8)

        self.preview_floating_bar = QtWidgets.QWidget(card)
        self.preview_floating_bar.setObjectName("PreviewFloatingBar")
        floating_layout = QtWidgets.QHBoxLayout(self.preview_floating_bar)
        floating_layout.setContentsMargins(0, 0, 0, 0)
        floating_layout.setSpacing(0)

        self.preview_toggle_button = ChevronToolButton(self.preview_floating_bar)
        self.preview_toggle_button.setObjectName("PreviewToggleButton")
        self.preview_toggle_button.setProperty("active", False)
        self.preview_toggle_button.setCheckable(True)
        self.preview_toggle_button.setToolTip("收起双预览")
        self.preview_toggle_button.toggled.connect(self._toggle_preview_panels)
        floating_layout.addWidget(self.preview_toggle_button)
        self.preview_floating_bar.adjustSize()
        self.preview_floating_bar.raise_()

        self.preview_compact_bar = QtWidgets.QFrame()
        self.preview_compact_bar.setObjectName("PreviewCompactBar")
        self.preview_compact_bar.setFixedHeight(44)
        compact_layout = QtWidgets.QHBoxLayout(self.preview_compact_bar)
        compact_layout.setContentsMargins(16, 0, 40, 0)
        compact_layout.setSpacing(0)

        self.preview_compact_summary = QtWidgets.QLabel()
        self.preview_compact_summary.setObjectName("PreviewCompactSummary")
        self.preview_compact_summary.setWordWrap(False)
        self.preview_compact_summary.setAlignment(QtCore.Qt.AlignVCenter | QtCore.Qt.AlignLeft)
        compact_layout.addWidget(self.preview_compact_summary, 1)
        self.preview_compact_bar.setVisible(False)
        layout.addWidget(self.preview_compact_bar)

        self.preview_content_widget = QtWidgets.QWidget()
        preview_content_layout = QtWidgets.QVBoxLayout(self.preview_content_widget)
        preview_content_layout.setContentsMargins(0, 0, 0, 0)
        preview_content_layout.setSpacing(0)

        preview_row = QtWidgets.QHBoxLayout()
        preview_row.setContentsMargins(0, 0, 0, 0)
        preview_row.setSpacing(10)

        left_preview = self._create_preview_panel("原图", "等待生成或导入图片")
        right_preview = self._create_result_preview_panel()

        self.original_image_label = left_preview[1]
        self.contour_image_label = right_preview[1]

        preview_row.addWidget(left_preview[0], 1)
        preview_row.addWidget(right_preview[0], 1)
        preview_content_layout.addLayout(preview_row)
        layout.addWidget(self.preview_content_widget, 1)

        self._position_preview_floating_bar()
        self._set_preview_toolbar_hovered(False)
        self._refresh_preview_summary()
        return card

    def _refresh_preview_summary(self) -> None:
        original_ready = bool(self.current_image_path)
        contour_ready = bool(self.current_contours_path)
        gcode_ready = bool(self.current_gcode_lines)
        if original_ready and contour_ready and gcode_ready:
            summary = "原图、轮廓与 G-Code 轨迹已就绪"
        elif original_ready and contour_ready:
            summary = "原图与轮廓已就绪，待生成 G-Code"
        elif original_ready:
            summary = "原图已就绪，待轮廓"
        else:
            summary = "等待导入或生成"
        self.preview_compact_summary.setText(summary)

    def _append_status_separator(self, layout: QtWidgets.QHBoxLayout) -> None:
        separator = QtWidgets.QLabel("|")
        separator.setObjectName("StatusInlineSeparator")
        separator.setAlignment(QtCore.Qt.AlignCenter)
        separator.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Preferred)
        layout.addWidget(separator)

    def _append_live_status_field(
        self,
        layout: QtWidgets.QHBoxLayout,
        title: str,
        *,
        stretch: int = 0,
        elide: bool = False,
    ) -> QtWidgets.QLabel:
        container = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(container)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(4)

        title_label = QtWidgets.QLabel(f"{title}:")
        title_label.setObjectName("StatusInlineKey")
        title_label.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Preferred)

        if elide:
            value_label = ElidedLabel("--")
            value_label.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Preferred)
        else:
            value_label = QtWidgets.QLabel("--")
            value_label.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Preferred)
        value_label.setObjectName("StatusInlineValue")
        value_label.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
        value_label.setWordWrap(False)

        row.addWidget(title_label)
        row.addWidget(value_label, 1)
        layout.addWidget(container, stretch)
        return value_label

    def _create_preview_panel(self, title: str, placeholder: str) -> Tuple[QtWidgets.QFrame, ImageLabel]:
        panel = QtWidgets.QFrame()
        panel.setObjectName("PreviewPanel")
        panel_layout = QtWidgets.QVBoxLayout(panel)
        panel_layout.setContentsMargins(10, 10, 10, 10)
        panel_layout.setSpacing(6)

        title_label = QtWidgets.QLabel(title)
        title_label.setObjectName("PreviewTitle")
        panel_layout.addWidget(title_label)

        image_label = ImageLabel(placeholder)
        panel_layout.addWidget(image_label, 1)
        return panel, image_label

    def _create_result_preview_panel(self) -> Tuple[QtWidgets.QFrame, ImageLabel]:
        panel = QtWidgets.QFrame()
        panel.setObjectName("PreviewPanel")
        panel_layout = QtWidgets.QVBoxLayout(panel)
        panel_layout.setContentsMargins(10, 10, 10, 10)
        panel_layout.setSpacing(6)

        title_row = QtWidgets.QHBoxLayout()
        title_row.setContentsMargins(0, 0, 0, 0)
        title_row.setSpacing(8)
        title_label = QtWidgets.QLabel("轮廓")
        title_label.setObjectName("PreviewTitle")
        title_row.addWidget(title_label)
        title_row.addStretch(1)

        self.result_preview_mode_combo = ClickToSelectComboBox()
        self.result_preview_mode_combo.addItem("轮廓", "contour")
        self.result_preview_mode_combo.addItem("G-Code", "gcode")
        self.result_preview_mode_combo.addItem("叠加", "overlay")
        self.result_preview_mode_combo.setMaximumWidth(128)
        self.result_preview_mode_combo.currentIndexChanged.connect(self._on_result_preview_mode_changed)
        title_row.addWidget(self.result_preview_mode_combo)
        panel_layout.addLayout(title_row)

        self.result_preview_stack = QtWidgets.QStackedWidget()
        contour_label = ImageLabel("等待轮廓提取结果")
        self.gcode_preview_widget = GCodePreviewWidget()
        self.gcode_overlay_widget = GCodePreviewWidget()
        self.result_preview_stack.addWidget(contour_label)
        self.result_preview_stack.addWidget(self.gcode_preview_widget)
        self.result_preview_stack.addWidget(self.gcode_overlay_widget)
        panel_layout.addWidget(self.result_preview_stack, 1)
        return panel, contour_label

    def _on_result_preview_mode_changed(self) -> None:
        if not hasattr(self, "result_preview_stack") or not hasattr(self, "result_preview_mode_combo"):
            return
        mode = str(self.result_preview_mode_combo.currentData() or "contour")
        index = {"contour": 0, "gcode": 1, "overlay": 2}.get(mode, 0)
        self.result_preview_stack.setCurrentIndex(index)

    def _build_log_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("📝 系统消息板", "这里会显示串口或 WiFi 收发、G-Code 预览和任务运行状态，便于比赛演示。")
        card.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        card.setMinimumHeight(180)

        self.log_edit = QtWidgets.QTextEdit()
        self.log_edit.setReadOnly(True)
        self.log_edit.setObjectName("LogBox")
        self.log_edit.setFrameShape(QtWidgets.QFrame.NoFrame)
        self.log_edit.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        layout.addWidget(self.log_edit, 1)
        return card

    def _position_preview_floating_bar(self) -> None:
        if not hasattr(self, "preview_card") or not hasattr(self, "preview_floating_bar"):
            return
        self.preview_floating_bar.adjustSize()
        x = max(8, self.preview_card.width() - self.preview_floating_bar.width() - 10)
        y = 10
        if hasattr(self, "preview_toggle_button") and self.preview_toggle_button.isChecked():
            y = max(10, (self.preview_card.height() - self.preview_floating_bar.height()) // 2)
        self.preview_floating_bar.move(x, y)
        self.preview_floating_bar.raise_()

    def _refresh_preview_card_style(self) -> None:
        if not hasattr(self, "preview_card"):
            return
        self.preview_card.style().unpolish(self.preview_card)
        self.preview_card.style().polish(self.preview_card)
        self.preview_card.update()

    def _set_preview_toolbar_hovered(self, hovered: bool) -> None:
        if not hasattr(self, "preview_toggle_button") or not hasattr(self, "preview_card"):
            return
        self.preview_card.setProperty("previewHover", hovered)
        active = hovered or self.preview_toggle_button.isChecked()
        self.preview_toggle_button.setProperty("active", active)
        self.preview_floating_bar.setVisible(active)
        self._refresh_preview_card_style()
        self.preview_toggle_button.style().unpolish(self.preview_toggle_button)
        self.preview_toggle_button.style().polish(self.preview_toggle_button)
        self.preview_toggle_button.update()
        self._position_preview_floating_bar()

    def _on_preview_card_clicked(self) -> None:
        if hasattr(self, "preview_toggle_button") and self.preview_toggle_button.isChecked():
            self.preview_toggle_button.setChecked(False)

    def _toggle_preview_panels(self, collapsed: bool) -> None:
        if not hasattr(self, "preview_content_widget"):
            return
        self.preview_content_widget.setVisible(not collapsed)
        self.preview_compact_bar.setVisible(collapsed)
        if hasattr(self, "preview_card"):
            self.preview_card.setProperty("previewCollapsed", collapsed)
            self.preview_card.setMinimumHeight(46 if collapsed else 240)
            self.preview_card.setMaximumHeight(48 if collapsed else 16777215)
        if hasattr(self, "preview_card_layout"):
            if collapsed:
                self.preview_card_layout.setContentsMargins(0, 0, 0, 0)
            else:
                self.preview_card_layout.setContentsMargins(16, 14, 16, 14)
        self.preview_toggle_button.setToolTip("展开双预览" if collapsed else "收起双预览")
        hovered = self.preview_card.underMouse() if hasattr(self, "preview_card") else False
        self._set_preview_toolbar_hovered(hovered)
        self._position_preview_floating_bar()
        if hasattr(self, "right_splitter"):
            if collapsed:
                self.right_splitter.setSizes([74, 626])
            else:
                self.right_splitter.setSizes([520, 200])

    def apply_stylesheet(self) -> None:
        self.setStyleSheet(BRIGHT_MAKER_QSS)

    def _current_transport_mode(self) -> str:
        return str(self.transport_combo.currentData() or "serial")

    def _has_selectable_serial_port(self) -> bool:
        if not hasattr(self, "port_combo") or self.port_combo.count() == 0:
            return False
        return bool(self.port_combo.currentData())

    def _update_connection_ui(self, connected: bool) -> None:
        """根据设备连接状态更新相关按钮可用性，避免误操作。"""

        mode = self._current_transport_mode()
        serial_available = self._has_selectable_serial_port()
        if hasattr(self, "preview_button"):
            self.preview_button.setEnabled(connected)
        if hasattr(self, "start_button"):
            self.start_button.setEnabled(connected)
        if hasattr(self, "emergency_button"):
            self.emergency_button.setEnabled(connected)
        for preset_button in (
            getattr(self, "serial_preset_button", None),
            getattr(self, "wifi_softap_button", None),
            getattr(self, "wifi_sta_button", None),
        ):
            if preset_button is not None:
                preset_button.setEnabled(not connected)
        self.transport_combo.setEnabled(not connected)
        self.serial_config_widget.setVisible(mode == "serial")
        self.wifi_config_widget.setVisible(mode == "wifi")
        self.port_combo.setEnabled((mode == "serial") and (not connected) and serial_available)
        self.baud_combo.setEnabled((mode == "serial") and (not connected))
        self.refresh_ports_button.setEnabled((mode == "serial") and (not connected))
        self.wifi_host_edit.setEnabled((mode == "wifi") and (not connected))
        self.wifi_port_spin.setEnabled((mode == "wifi") and (not connected))
        if connected:
            self.connect_button.setEnabled(True)
            self.connect_button.setText("断开设备")
            if mode == "wifi":
                self.connection_summary_label.setText(
                    f"当前已通过 {format_transport_label(mode)} 连接：{self.serial_thread.connection_description()}，"
                    f"工作流预设为 {self._current_wifi_workflow_label()}。"
                )
            else:
                self.connection_summary_label.setText(
                    f"当前已通过 {format_transport_label(mode)} 连接：{self.serial_thread.connection_description()}"
                )
            self.live_state_value.setText("已连接")
            self.live_target_value.setText(self.serial_thread.connection_description() or "--")
            self.live_target_value.setToolTip(self.serial_thread.connection_description() or "--")
            self.live_transport_value.setText(format_transport_label(mode))
            if mode == "wifi":
                self.live_wifi_mode_value.setText(self._current_wifi_workflow_label().replace("WiFi ", ""))
                self.live_sle_value.setText("未知")
            else:
                self.live_wifi_mode_value.setText("--")
                self.live_sle_value.setText("--")
        else:
            self.connect_button.setText("连接设备")
            if mode == "wifi":
                self.connect_button.setEnabled(True)
                preset = WIFI_WORKFLOW_PRESETS.get(self._current_wifi_workflow, WIFI_WORKFLOW_PRESETS["softap"])
                self.connection_summary_label.setText(
                    f"当前选择 {self._current_wifi_workflow_label()}。{preset['summary']}"
                )
            else:
                self.connect_button.setEnabled(serial_available)
                if serial_available:
                    self.connection_summary_label.setText("当前选择串口 UART，可连接发射板业务串口。")
                else:
                    self.connection_summary_label.setText("当前未检测到可用串口，请检查 USB 连接、驱动或权限后点击“刷新串口”。")
            self.reset_live_status_strip()

    def _ensure_device_connected(self, action_name: str) -> bool:
        """在发起真实下发任务前做界面层前置校验。"""

        if self.serial_thread.is_connected():
            return True
        mode_name = "WiFi TCP" if self._current_transport_mode() == "wifi" else "串口 UART"
        message = f"请先连接设备（当前模式：{mode_name}），再执行“{action_name}”。"
        self.append_log(f"[提示] {message}")
        QtWidgets.QMessageBox.information(self, "请先连接设备", message)
        return False

    def _current_baud_rate(self) -> Optional[int]:
        text = self.baud_combo.currentText().strip()
        if not text:
            QtWidgets.QMessageBox.warning(self, "提示", "请输入有效的波特率。")
            return None
        try:
            value = int(text)
        except ValueError:
            QtWidgets.QMessageBox.warning(self, "提示", "波特率必须是正整数。")
            return None
        if value <= 0:
            QtWidgets.QMessageBox.warning(self, "提示", "波特率必须大于 0。")
            return None
        return value

    def _confirm_device_job(self, title: str, *, line_count: int = 0) -> bool:
        connection = self.serial_thread.connection_description() or self._current_connection_target()
        mode = format_transport_label(self._current_transport_mode())
        extra = ""
        if self._current_transport_mode() == "wifi" and self._current_wifi_workflow == "softap":
            extra = "\n当前为 WiFi SoftAP，若电脑没有其他外网链路，在线 AI 生图可能不可用。"
        line_text = f"\nG-Code 行数：{line_count}" if line_count > 0 else ""
        message = (
            f"即将执行：{title}\n"
            f"链路：{mode} -> {connection}\n"
            f"尺寸：{self.width_spin.value():.1f} x {self.height_spin.value():.1f} mm\n"
            f"功率/进给：S{self.power_spin.value()} / F{self.feed_spin.value()}"
            f"{line_text}{extra}\n\n确认继续？"
        )
        answer = QtWidgets.QMessageBox.question(self, "执行确认", message)
        return answer == QtWidgets.QMessageBox.Yes

    def on_transport_mode_changed(self) -> None:
        self.serial_thread.set_transport_mode(self._current_transport_mode())
        if self._current_transport_mode() == "wifi":
            preset = WIFI_WORKFLOW_PRESETS.get(self._current_wifi_workflow, WIFI_WORKFLOW_PRESETS["softap"])
            self.wifi_host_edit.setPlaceholderText(str(preset["placeholder"]))
        self._update_connection_ui(self.serial_thread.is_connected())

    def _apply_connection_preset(self, preset: str) -> None:
        if preset == "serial":
            serial_index = self.transport_combo.findData("serial")
            if serial_index >= 0:
                self.transport_combo.setCurrentIndex(serial_index)
            self._update_connection_ui(self.serial_thread.is_connected())
            return

        self._current_wifi_workflow = "sta" if preset == "sta" else "softap"
        wifi_index = self.transport_combo.findData("wifi")
        if wifi_index >= 0:
            self.transport_combo.setCurrentIndex(wifi_index)

        workflow_preset = WIFI_WORKFLOW_PRESETS[self._current_wifi_workflow]
        self.wifi_host_edit.setText(str(workflow_preset["host"]))
        self.wifi_host_edit.setPlaceholderText(str(workflow_preset["placeholder"]))
        self.wifi_port_spin.setValue(int(workflow_preset["port"]))
        self._update_connection_ui(self.serial_thread.is_connected())

    def _current_wifi_workflow_label(self) -> str:
        return format_wifi_workflow_label(self._current_wifi_workflow)

    def reset_live_status_strip(self) -> None:
        if not hasattr(self, "live_state_value"):
            return
        self.live_state_value.setText("未连接")
        self.live_target_value.setText("--")
        self.live_target_value.setToolTip("")
        self.live_transport_value.setText(format_transport_label(self._current_transport_mode()))
        if self._current_transport_mode() == "wifi":
            self.live_wifi_mode_value.setText(self._current_wifi_workflow_label().replace("WiFi ", ""))
        else:
            self.live_wifi_mode_value.setText("--")
        self.live_sle_value.setText("--")
        self.live_coord_value.setText("--, --")
        self.live_feed_value.setText("--")
        self.live_power_value.setText("--")
        self.live_refresh_value.setText("--")

    def on_live_status_changed(self, snapshot: Dict[str, object]) -> None:
        if not hasattr(self, "live_state_value"):
            return

        raw_state = str(snapshot.get("state") or "--")
        state_text = LIVE_STATE_DISPLAY.get(raw_state, raw_state)
        x = float(snapshot.get("x", 0.0))
        y = float(snapshot.get("y", 0.0))
        feed = float(snapshot.get("feed", 0.0))
        power = float(snapshot.get("spindle", 0.0))
        endpoint = str(snapshot.get("endpoint") or self.serial_thread.connection_description() or "--")
        transport = str(snapshot.get("transport") or self._current_transport_mode() or "").strip()
        wifi_status = snapshot.get("wifi")
        wifi_mode_text = "--"
        sle_text = "--"
        if transport == "wifi":
            if isinstance(wifi_status, dict):
                wifi_mode_text = str(wifi_status.get("mode") or "").strip() or self._current_wifi_workflow_label().replace(
                    "WiFi ", ""
                )
                sle_ready = wifi_status.get("sle_ready")
                if isinstance(sle_ready, bool):
                    sle_text = "就绪" if sle_ready else "未就绪"
                else:
                    sle_text = "未知"
            else:
                wifi_mode_text = self._current_wifi_workflow_label().replace("WiFi ", "")
                sle_text = "未知"
        refresh_text = datetime.now().strftime("%H:%M:%S")

        self.live_state_value.setText(state_text)
        self.live_target_value.setText(endpoint)
        self.live_target_value.setToolTip(endpoint)
        self.live_transport_value.setText(format_transport_label(transport))
        self.live_wifi_mode_value.setText(wifi_mode_text)
        self.live_sle_value.setText(sle_text)
        self.live_coord_value.setText(f"{x:.3f}, {y:.3f}")
        self.live_feed_value.setText(f"{feed:.0f}")
        self.live_power_value.setText(f"{power:.0f}")
        self.live_refresh_value.setText(refresh_text)
        self.task_manager.update_live_status(snapshot)

    def _current_connection_target(self) -> str:
        if self.serial_thread.is_connected():
            return self.serial_thread.connection_description() or "--"
        if self._current_transport_mode() == "wifi":
            host = self.wifi_host_edit.text().strip() or "--"
            return f"{host}:{self.wifi_port_spin.value()}"
        return str(self.port_combo.currentData() or "--")

    def _begin_task(self, source_type: str) -> None:
        self.task_manager.begin_task(
            prompt=self.prompt_text_edit.toPlainText().strip(),
            image_model=self._current_image_model_label(),
            source_type=source_type,
            width_mm=float(self.width_spin.value()),
            height_mm=float(self.height_spin.value()),
            feed_rate=int(self.feed_spin.value()),
            power=int(self.power_spin.value()),
            connection_mode=self._current_transport_mode(),
            connection_target=self._current_connection_target(),
            wifi_workflow=self._current_wifi_workflow if self._current_transport_mode() == "wifi" else "",
        )

    def _rebuild_task_up_to_gcode_stage(self) -> None:
        source_type = self.current_source_type if self.current_source_type in ("ai", "local") else "local"
        self._begin_task(source_type)
        if self.current_image_path:
            if source_type == "ai":
                self.task_manager.on_ai_image_ready(self.current_image_path)
            else:
                self.task_manager.on_local_image_loaded(self.current_image_path)
        if self.current_contours_path:
            self.task_manager.on_contour_ready(self.current_contours_path, len(self.current_contours))

    def _ensure_task_for_gcode(self) -> None:
        if self.task_manager.state == TaskState.SENDING and not self.serial_thread.is_sending():
            self._rebuild_task_up_to_gcode_stage()
            return
        if self.task_manager.current_record is not None and self.task_manager.state not in (
            TaskState.IDLE,
            TaskState.COMPLETED,
            TaskState.FAILED,
        ):
            return
        self._rebuild_task_up_to_gcode_stage()

    def _ensure_task_for_sending(self) -> None:
        self._ensure_task_for_gcode()
        self.task_manager.update_connection_context(
            connection_mode=self._current_transport_mode(),
            connection_target=self._current_connection_target(),
            wifi_workflow=self._current_wifi_workflow if self._current_transport_mode() == "wifi" else "",
        )
        if self.current_gcode_lines and self.task_manager.state != TaskState.SENDING:
            self.task_manager.on_gcode_ready(self.current_gcode_lines)

    def _task_status_text(self) -> str:
        state = self.task_manager.state
        if state == TaskState.GCODE_GENERATING and not self._is_generating_gcode and not self.current_gcode_lines:
            return "当前状态: 轮廓已就绪，等待生成 G-Code"
        if state == TaskState.SENDING and not self.serial_thread.is_sending():
            return "当前状态: G-Code 已就绪，等待下发"
        return f"当前状态: {self.task_manager.state_display}"

    def _task_detail_text(self) -> str:
        state = self.task_manager.state
        record = self.task_manager.current_record
        if record is None or state == TaskState.IDLE:
            return "当前系统空闲，等待发起 AI 生图或导入本地文件。"

        total_lines = record.total_lines or record.gcode_line_count
        if state == TaskState.AI_GENERATING:
            return "AI 生图中，正在等待云端返回图像结果。"
        if state == TaskState.CONTOUR_EXTRACTING:
            return "轮廓提取中，正在进行降噪、边缘提取与归一化。"
        if state == TaskState.GCODE_GENERATING and not self._is_generating_gcode and not self.current_gcode_lines:
            return "轮廓已就绪，等待生成 G-Code 或调整参数后继续。"
        if state == TaskState.GCODE_GENERATING:
            return "G-Code 生成中，正在将轮廓映射到物理坐标。"
        if state == TaskState.SENDING and not self.serial_thread.is_sending():
            if total_lines > 0:
                return f"G-Code 已就绪，共 {total_lines} 行，等待下发到 WS63。"
            return "G-Code 已就绪，等待下发到 WS63。"
        if state == TaskState.SENDING:
            progress_text = f"已完成 {record.sent_lines}/{total_lines} 行" if total_lines > 0 else "正在下发 G-Code"
            if record.last_completed_line:
                return f"下发中，{progress_text}；最近成功 {record.last_completed_line}"
            return f"下发中，{progress_text}。"
        if state == TaskState.COMPLETED:
            duration_text = f"，用时 {record.duration_seconds:.1f} 秒" if record.duration_seconds > 0 else ""
            if total_lines > 0:
                return f"任务执行完成，共处理 {total_lines} 行 G-Code{duration_text}。"
            return f"任务执行完成{duration_text}。"
        if state == TaskState.FAILED:
            reason = record.last_stop_reason or record.error_message
            if reason:
                return f"任务已停止：{reason}"
            return "任务执行失败，请检查日志后重试。"
        return self.task_manager.state_detail

    def _refresh_task_status_panel(self) -> None:
        if not hasattr(self, "progress_bar"):
            return
        self.task_status_label.setText(self._task_status_text())
        self.task_detail_label.setText(self._task_detail_text())
        if self.task_manager.state != TaskState.SENDING or not self.serial_thread.is_sending():
            self.progress_bar.setValue(self.task_manager.state_progress)

    def _on_task_state_changed(self, _task_manager: TaskManager) -> None:
        self._refresh_task_status_panel()

    def _show_task_error(self, title: str, raw_message: str, *, mark_failed: bool = True) -> str:
        friendly_message = self.task_manager.get_readable_error(raw_message)
        if mark_failed and self.task_manager.current_record is not None and self.task_manager.state not in (
            TaskState.IDLE,
            TaskState.COMPLETED,
            TaskState.FAILED,
        ):
            self.task_manager.on_task_failed(friendly_message)
        self.append_log(f"[错误] {friendly_message}", color="#C53030", bold=True)
        if mark_failed and self.task_manager.current_record is not None and self.task_manager.state == TaskState.FAILED:
            self._archive_task_to_history()
        QtWidgets.QMessageBox.warning(self, title, friendly_message)
        return friendly_message

    def append_log(self, message: str, color: Optional[str] = None, bold: bool = False) -> None:
        cursor = self.log_edit.textCursor()
        cursor.movePosition(QtGui.QTextCursor.End)
        if color or bold:
            fmt = QtGui.QTextCharFormat()
            if color:
                fmt.setForeground(QtGui.QColor(color))
            if bold:
                fmt.setFontWeight(QtGui.QFont.Bold)
            cursor.insertText(message + "\n", fmt)
        else:
            cursor.insertText(message + "\n")
        self.log_edit.setTextCursor(cursor)
        self.log_edit.ensureCursorVisible()
        self.task_manager.append_log_line(message)

    def on_progress_changed(self, value: int) -> None:
        if self.task_manager.state != TaskState.SENDING:
            self._refresh_task_status_panel()
            return
        clamped = max(0, min(100, int(value)))
        base_progress = self.task_manager.state_progress
        mapped_progress = base_progress + int((99 - base_progress) * clamped / 100)
        self.progress_bar.setValue(mapped_progress)
        self.task_status_label.setText("当前状态: 📡 下发到设备中…")
        self.task_detail_label.setText(self._task_detail_text())

    def on_job_line_progress(self, payload: Dict[str, object]) -> None:
        sent = int(payload.get("sent", 0))
        total = int(payload.get("total", 0))
        current_line = str(payload.get("line", "")).strip()
        self.task_manager.on_sending_progress(sent, total, current_line=current_line)
        self._refresh_task_status_panel()

    def _clear_layout(self, layout: QtWidgets.QLayout) -> None:
        while layout.count():
            item = layout.takeAt(0)
            child_layout = item.layout()
            widget = item.widget()
            if child_layout is not None:
                self._clear_layout(child_layout)
            if widget is not None:
                widget.deleteLater()

    def _history_summary_text(self, meta: Dict[str, Any], history_dir: Path) -> str:
        timestamp = str(meta.get("completed_at") or meta.get("timestamp") or history_dir.name)
        try:
            display_time = datetime.fromisoformat(timestamp).strftime("%Y-%m-%d %H:%M")
        except ValueError:
            try:
                display_time = datetime.strptime(timestamp, "%Y%m%d_%H%M%S").strftime("%Y-%m-%d %H:%M")
            except ValueError:
                display_time = timestamp
        status = str(meta.get("status", "")).strip().lower()
        status_text = "❌ 失败" if status == "failed" else "✅ 成功" if status == "success" else "⏳ 处理中"
        width_mm = float(meta.get("width_mm", 0.0))
        height_mm = float(meta.get("height_mm", 0.0))
        power = int(meta.get("power", 0))
        feed_rate = int(meta.get("feed_rate", 0))
        return f"{display_time}  ·  {status_text}  ·  {width_mm:.0f}x{height_mm:.0f} mm  ·  功率 {power}  ·  F{feed_rate}"

    def _open_local_directory(self, target_dir: Path, title: str = "目录") -> None:
        if not target_dir.exists():
            QtWidgets.QMessageBox.warning(self, title, f"目录不存在：{target_dir}")
            return
        opened = QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(str(target_dir)))
        if not opened:
            QtWidgets.QMessageBox.warning(self, title, f"无法打开目录：{target_dir}")

    def _show_history_details(self, history_dir_path: str) -> None:
        history_dir = Path(history_dir_path)
        meta_path = history_dir / "meta.json"
        log_path = history_dir / "task.log"

        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {}
        except (OSError, json.JSONDecodeError) as exc:
            QtWidgets.QMessageBox.warning(self, "任务详情读取失败", f"无法读取任务元数据：{exc}")
            return

        try:
            log_text = log_path.read_text(encoding="utf-8") if log_path.exists() else "暂无归档日志。"
        except OSError as exc:
            log_text = f"日志读取失败：{exc}"

        sle_ready = meta.get("sle_ready")
        if isinstance(sle_ready, bool):
            sle_ready_text = "是" if sle_ready else "否"
        else:
            sle_ready_text = "--"

        dialog = QtWidgets.QDialog(self)
        dialog.setWindowTitle(f"任务详情 - {meta.get('task_id', history_dir.name)}")
        dialog.resize(760, 620)

        layout = QtWidgets.QVBoxLayout(dialog)
        layout.setContentsMargins(16, 16, 16, 16)
        layout.setSpacing(10)

        summary_title = QtWidgets.QLabel("任务摘要")
        summary_title.setStyleSheet("font-size:15px; font-weight:800; color:#2D3748;")
        layout.addWidget(summary_title)

        summary_lines = [
            f"任务编号：{meta.get('task_id', history_dir.name)}",
            f"状态：{meta.get('status', '--')}",
            f"创建时间：{meta.get('timestamp', '--')}",
            f"完成时间：{meta.get('completed_at', '--')}",
            f"链路：{format_transport_label(str(meta.get('connection_mode', '')).strip())}",
            f"目标：{meta.get('connection_target') or '--'}",
            f"WiFi 工作流：{meta.get('wifi_workflow') or '--'}",
            f"WiFi 模式：{meta.get('wifi_mode') or '--'}",
            f"SLE 就绪：{sle_ready_text}",
            f"尺寸：{float(meta.get('width_mm', 0.0)):.1f} x {float(meta.get('height_mm', 0.0)):.1f} mm",
            f"功率/进给：S{int(meta.get('power', 0))} / F{int(meta.get('feed_rate', 0))}",
            f"G-Code 行数：{int(meta.get('gcode_line_count', 0))}",
            f"已完成行数：{int(meta.get('sent_lines', 0))}/{int(meta.get('total_lines', 0) or meta.get('gcode_line_count', 0))}",
            f"最近成功指令：{meta.get('last_completed_line') or '--'}",
            f"停止原因：{meta.get('last_stop_reason') or '--'}",
            f"失败原因：{meta.get('error_msg') or '--'}",
            f"Prompt：{meta.get('prompt') or '--'}",
        ]
        summary_edit = QtWidgets.QTextEdit()
        summary_edit.setReadOnly(True)
        summary_edit.setObjectName("LogBox")
        summary_edit.setMinimumHeight(220)
        summary_edit.setPlainText("\n".join(summary_lines))
        layout.addWidget(summary_edit)

        log_title = QtWidgets.QLabel("归档日志")
        log_title.setStyleSheet("font-size:15px; font-weight:800; color:#2D3748;")
        layout.addWidget(log_title)

        log_edit = QtWidgets.QTextEdit()
        log_edit.setReadOnly(True)
        log_edit.setObjectName("LogBox")
        log_edit.setPlainText(log_text)
        layout.addWidget(log_edit, 1)

        button_row = QtWidgets.QHBoxLayout()
        button_row.addStretch(1)
        open_dir_button = QtWidgets.QPushButton("打开目录")
        open_dir_button.setProperty("secondary", True)
        open_dir_button.clicked.connect(lambda: self._open_local_directory(history_dir, "任务产物目录"))
        close_button = QtWidgets.QPushButton("关闭")
        close_button.clicked.connect(dialog.accept)
        button_row.addWidget(open_dir_button)
        button_row.addWidget(close_button)
        layout.addLayout(button_row)

        exec_method = getattr(dialog, "exec", None)
        if callable(exec_method):
            exec_method()
            return
        exec_legacy = getattr(dialog, "exec_", None)
        if callable(exec_legacy):
            exec_legacy()

    def _load_history_prompt(self, history_dir_path: str) -> None:
        history_dir = Path(history_dir_path)
        meta_path = history_dir / "meta.json"
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {}
        except (OSError, json.JSONDecodeError) as exc:
            QtWidgets.QMessageBox.warning(self, "历史任务读取失败", f"无法读取任务参数：{exc}")
            return

        prompt = str(meta.get("prompt", "")).strip()
        if not prompt:
            QtWidgets.QMessageBox.warning(self, "历史任务读取失败", "该历史任务没有可复用的 Prompt。")
            return
        self.prompt_text_edit.setPlainText(prompt)
        saved_model = str(meta.get("image_model", "")).strip()
        if saved_model:
            index = self.image_model_combo.findData(saved_model)
            if index >= 0:
                self.image_model_combo.setCurrentIndex(index)
        self.left_tabs.setCurrentIndex(1)
        self.append_log(f"已复用历史 Prompt：{history_dir.name}")

    def _delete_history_task(self, history_dir_path: str) -> None:
        history_dir = Path(history_dir_path)
        answer = QtWidgets.QMessageBox.question(
            self,
            "删除历史任务",
            f"确认删除历史任务 {history_dir.name}？该操作会移除本地归档文件。",
        )
        if answer != QtWidgets.QMessageBox.Yes:
            return
        try:
            shutil.rmtree(history_dir)
        except OSError as exc:
            QtWidgets.QMessageBox.warning(self, "删除失败", f"无法删除历史任务：{exc}")
            return
        if self.current_history_dir == history_dir:
            self.current_history_dir = None
        self._refresh_history_gallery()
        self.append_log(f"已删除历史任务：{history_dir.name}")

    def _build_history_thumbnail(self, image_path: Path, size: int = 60, radius: int = 10) -> QtGui.QPixmap:
        pixmap = QtGui.QPixmap(str(image_path))
        if pixmap.isNull():
            placeholder = QtGui.QPixmap(size, size)
            placeholder.fill(QtGui.QColor("#EEF3F8"))
            painter = QtGui.QPainter(placeholder)
            painter.setPen(QtGui.QColor("#94A3B8"))
            painter.drawText(placeholder.rect(), QtCore.Qt.AlignCenter, "图")
            painter.end()
            return placeholder

        scaled = pixmap.scaled(
            size,
            size,
            QtCore.Qt.KeepAspectRatioByExpanding,
            QtCore.Qt.SmoothTransformation,
        )
        rounded = QtGui.QPixmap(size, size)
        rounded.fill(QtCore.Qt.transparent)

        painter = QtGui.QPainter(rounded)
        painter.setRenderHint(QtGui.QPainter.Antialiasing, True)
        clip_path = QtGui.QPainterPath()
        clip_path.addRoundedRect(QtCore.QRectF(0, 0, size, size), radius, radius)
        painter.setClipPath(clip_path)
        offset_x = (size - scaled.width()) // 2
        offset_y = (size - scaled.height()) // 2
        painter.drawPixmap(offset_x, offset_y, scaled)
        painter.end()
        return rounded

    def _create_history_card(self, history_dir: Path, meta: Dict[str, Any]) -> QtWidgets.QFrame:
        card = QtWidgets.QFrame()
        card.setObjectName("HistoryCard")
        status = str(meta.get("status", "")).strip().lower()
        error_msg = str(meta.get("error_msg", "")).strip()
        has_error = status == "failed" and bool(error_msg)
        card.setFixedHeight(152 if has_error else 132)
        layout = QtWidgets.QHBoxLayout(card)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(10)

        thumb_label = QtWidgets.QLabel()
        thumb_label.setFixedSize(60, 60)
        thumb_label.setAlignment(QtCore.Qt.AlignCenter)
        thumb_label.setStyleSheet("background:#EEF3F8; border-radius:10px;")
        thumb_path = history_dir / "original.png"
        if not thumb_path.exists():
            thumb_path = history_dir / "contour.png"
        thumb_label.setPixmap(self._build_history_thumbnail(thumb_path))
        layout.addWidget(thumb_label, 0, QtCore.Qt.AlignTop)

        text_layout = QtWidgets.QVBoxLayout()
        text_layout.setContentsMargins(0, 0, 0, 0)
        text_layout.setSpacing(4)

        prompt_text = str(meta.get("prompt", "")).strip().replace("\n", " ")
        prompt_label = ElidedLabel(prompt_text or "未命名任务")
        prompt_label.setStyleSheet("font-weight:700; color:#2D3748;")
        text_layout.addWidget(prompt_label)

        meta_label = QtWidgets.QLabel(self._history_summary_text(meta, history_dir))
        meta_label.setObjectName("HistoryMeta")
        meta_label.setWordWrap(False)
        text_layout.addWidget(meta_label)
        if has_error:
            error_label = ElidedLabel(f"失败原因：{error_msg}")
            error_label.setStyleSheet("font-weight:400; color:#C53030;")
            text_layout.addWidget(error_label)
        text_layout.addStretch(1)
        layout.addLayout(text_layout, 1)

        button_layout = QtWidgets.QVBoxLayout()
        button_layout.setContentsMargins(0, 0, 0, 0)
        button_layout.setSpacing(6)

        detail_button = QtWidgets.QPushButton("详情")
        detail_button.setProperty("secondary", True)
        detail_button.setCursor(QtCore.Qt.PointingHandCursor)
        detail_button.clicked.connect(lambda _checked=False, path=str(history_dir): self._show_history_details(path))
        button_layout.addWidget(detail_button)

        reuse_button = QtWidgets.QPushButton("复用")
        reuse_button.setProperty("secondary", True)
        reuse_button.setCursor(QtCore.Qt.PointingHandCursor)
        gcode_exists = (history_dir / "task.gcode").exists()
        reuse_button.setEnabled(gcode_exists)
        if gcode_exists:
            reuse_button.clicked.connect(lambda _checked=False, path=str(history_dir): self._load_history_task(path))
        else:
            reuse_button.setToolTip("该任务未生成完整 G-Code，无法直接复用。")
        button_layout.addWidget(reuse_button)

        more_button = QtWidgets.QPushButton("更多")
        more_button.setProperty("secondary", True)
        more_button.setCursor(QtCore.Qt.PointingHandCursor)
        menu = QtWidgets.QMenu(more_button)
        open_action = menu.addAction("打开目录")
        open_action.triggered.connect(lambda _checked=False, path=history_dir: self._open_local_directory(path, "任务产物目录"))
        prompt_action = menu.addAction("仅复用 Prompt")
        prompt_action.triggered.connect(lambda _checked=False, path=str(history_dir): self._load_history_prompt(path))
        delete_action = menu.addAction("删除")
        delete_action.triggered.connect(lambda _checked=False, path=str(history_dir): self._delete_history_task(path))
        more_button.setMenu(menu)
        button_layout.addWidget(more_button)
        button_layout.addStretch(1)
        layout.addLayout(button_layout, 0)
        return card

    def _refresh_history_gallery(self) -> None:
        if not hasattr(self, "history_list_layout"):
            return
        self._clear_layout(self.history_list_layout)
        history_dirs = sorted((path for path in self.history_root.iterdir() if path.is_dir()), reverse=True)

        if not history_dirs:
            empty_label = QtWidgets.QLabel("历史图库暂时为空。任务成功或失败结束后，这里都会自动出现归档记录。")
            empty_label.setObjectName("HistoryEmptyState")
            empty_label.setWordWrap(True)
            self.history_list_layout.addWidget(empty_label)
            self.history_list_layout.addStretch(1)
            return

        for history_dir in history_dirs:
            meta_path = history_dir / "meta.json"
            if meta_path.exists():
                try:
                    meta = json.loads(meta_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    meta = {}
            else:
                meta = {}
            self.history_list_layout.addWidget(self._create_history_card(history_dir, meta))
        self.history_list_layout.addStretch(1)

    def _resolve_history_source(
        self,
        history_dir: Path,
        recorded_file: str,
        target_name: str,
    ) -> None:
        candidates: List[Path] = []
        current_task_dir = self.task_manager.current_task_dir
        recorded = recorded_file.strip()
        if recorded:
            recorded_path = Path(recorded)
            if not recorded_path.is_absolute() and current_task_dir is not None:
                recorded_path = current_task_dir / recorded_path
            candidates.append(recorded_path)

        for candidate in candidates:
            if candidate.exists():
                shutil.copy2(candidate, history_dir / target_name)
                return

    def _archive_task_to_history(self) -> None:
        record = self.task_manager.current_record
        if record is None:
            return

        history_dir = self.history_root / record.task_id
        try:
            history_dir.mkdir(parents=True, exist_ok=True)
            self._resolve_history_source(history_dir, record.ai_image_file, "original.png")
            self._resolve_history_source(history_dir, record.contour_image_file, "contour.png")
            self._resolve_history_source(history_dir, record.log_file or "task.log", "task.log")

            current_task_dir = self.task_manager.current_task_dir
            gcode_written = False
            if record.gcode_file and current_task_dir is not None:
                gcode_path = current_task_dir / record.gcode_file
                if gcode_path.exists():
                    shutil.copy2(gcode_path, history_dir / "task.gcode")
                    gcode_written = True
            if (not gcode_written) and record.gcode_line_count > 0 and self.current_gcode_lines:
                (history_dir / "task.gcode").write_text("\n".join(self.current_gcode_lines) + "\n", encoding="utf-8")

            meta = {
                "task_id": record.task_id,
                "timestamp": record.create_time,
                "completed_at": record.completed_at or record.finish_time,
                "status": record.status or ("success" if record.success else "failed"),
                "error_msg": record.error_msg or record.error_message,
                "prompt": record.prompt,
                "image_model": record.image_model or self._current_image_model_label(),
                "source_type": record.source_type or self.current_source_type,
                "width_mm": float(record.width_mm),
                "height_mm": float(record.height_mm),
                "feed_rate": int(record.feed_rate),
                "power": int(record.power),
                "connection_mode": record.connection_mode,
                "connection_target": record.connection_target,
                "wifi_workflow": record.wifi_workflow,
                "wifi_mode": record.wifi_mode,
                "sle_ready": record.sle_ready,
                "last_status_at": record.last_status_at,
                "gcode_line_count": int(record.gcode_line_count),
                "sent_lines": int(record.sent_lines),
                "total_lines": int(record.total_lines or record.gcode_line_count),
                "last_progress_percent": int(record.last_progress_percent),
                "last_completed_line": record.last_completed_line,
                "last_stop_reason": record.last_stop_reason,
                "log_file": record.log_file or "task.log",
                "duration_seconds": float(record.duration_seconds),
            }
            (history_dir / "meta.json").write_text(
                json.dumps(meta, ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
        except OSError as exc:
            self.append_log(f"[提示] 历史任务保存失败：{exc}")
            return

        self.current_history_dir = history_dir
        self._refresh_history_gallery()

    def _load_history_task(self, history_dir_path: str) -> None:
        history_dir = Path(history_dir_path)
        meta_path = history_dir / "meta.json"
        gcode_path = history_dir / "task.gcode"
        original_path = history_dir / "original.png"
        contour_path = history_dir / "contour.png"

        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {}
        except (OSError, json.JSONDecodeError) as exc:
            QtWidgets.QMessageBox.warning(self, "历史任务读取失败", f"无法读取任务参数：{exc}")
            return

        try:
            gcode_text = gcode_path.read_text(encoding="utf-8")
        except OSError as exc:
            QtWidgets.QMessageBox.warning(self, "历史任务读取失败", f"无法读取 G-Code：{exc}")
            return

        self.prompt_text_edit.setPlainText(str(meta.get("prompt", "")).strip())
        saved_model = str(meta.get("image_model", "")).strip()
        if saved_model:
            index = self.image_model_combo.findData(saved_model)
            if index >= 0:
                self.image_model_combo.setCurrentIndex(index)
        self.width_spin.setValue(float(meta.get("width_mm", self.width_spin.value())))
        self.height_spin.setValue(float(meta.get("height_mm", self.height_spin.value())))
        self.feed_spin.setValue(int(meta.get("feed_rate", self.feed_spin.value())))
        self.power_spin.setValue(int(meta.get("power", self.power_spin.value())))
        self._sync_material_preset_combo()

        self.current_image_path = str(original_path) if original_path.exists() else None
        self.current_contours_path = str(contour_path) if contour_path.exists() else None
        self.current_history_dir = history_dir
        self.current_source_type = str(meta.get("source_type", "local")).strip() or "local"
        self.current_preview_lines = []
        self.current_contours = []
        self.current_gcode_lines = [line for line in gcode_text.splitlines() if line.strip()]

        if self.current_image_path:
            try:
                restored_contours, _ = process_image_to_contours(
                    self.current_image_path,
                    **self._current_contour_processing_params(),
                )
                self.current_contours = restored_contours
            except (ImageProcessingError, OSError) as exc:
                self.append_log(f"[提示] 历史任务已载入，但轮廓路径未完全恢复：{exc}")

        if self.current_image_path:
            self.original_image_label.set_image(self.current_image_path)
        else:
            self.original_image_label.clear_to_placeholder("历史任务缺少原图")
        if self.current_contours_path:
            self.contour_image_label.set_image(self.current_contours_path)
        else:
            self.contour_image_label.clear_to_placeholder("历史任务缺少轮廓图")
        if hasattr(self, "gcode_preview_widget"):
            self.gcode_preview_widget.set_gcode_lines(self.current_gcode_lines)
        if hasattr(self, "gcode_overlay_widget"):
            self.gcode_overlay_widget.set_background_image(self.current_contours_path)
            self.gcode_overlay_widget.set_gcode_lines(self.current_gcode_lines)
        if hasattr(self, "result_preview_mode_combo") and self.current_gcode_lines:
            index = self.result_preview_mode_combo.findData("overlay")
            if index >= 0:
                self.result_preview_mode_combo.setCurrentIndex(index)

        self._refresh_preview_summary()
        self.task_manager.reset_to_idle()
        if hasattr(self, "machine_page"):
            self.left_tabs.setCurrentWidget(self.machine_page)
        self.append_log(f"已载入历史任务：{history_dir.name}")

    def update_ports(self, ports: List[Dict[str, str]]) -> None:
        current_device = self.port_combo.currentData()
        self.port_combo.blockSignals(True)
        self.port_combo.clear()
        if ports:
            for port in ports:
                device = str(port.get("device", "")).strip()
                display = str(port.get("display", device)).strip() or device
                suffix = f"({device})"
                if device and display.endswith(suffix):
                    display = display[:-len(suffix)].strip()
                description = str(port.get("description", "")).strip()
                self.port_combo.addItem(display, device)
                index = self.port_combo.count() - 1
                self.port_combo.setItemData(index, description, QtCore.Qt.ToolTipRole)
            if current_device:
                index = self.port_combo.findData(current_device)
                if index >= 0:
                    self.port_combo.setCurrentIndex(index)
        else:
            self.port_combo.addItem("未检测到串口", None)
        self.port_combo.blockSignals(False)
        self._update_connection_ui(self.serial_thread.is_connected())

    def refresh_ports(self) -> None:
        ports = self.serial_thread.list_available_ports()
        self.update_ports(ports)
        if ports:
            self.append_log(f"已刷新串口列表，共检测到 {len(ports)} 个串口。")
        else:
            self.append_log("[提示] 未检测到可用串口，请检查设备连接、驱动或当前用户权限。")

    def _set_ai_generating(self, generating: bool) -> None:
        self.generate_image_button.setDisabled(generating)
        self.competition_flow_button.setDisabled(generating)
        self.image_model_combo.setDisabled(generating)
        self.import_image_button.setDisabled(generating)
        if generating:
            self.generate_image_button.setText("✨ 生成中...")
            self.competition_flow_button.setText("⚡ 比赛流程执行中...")
        else:
            self.generate_image_button.setText("✨ 魔法生成图像")
            self.competition_flow_button.setText("⚡ 一键比赛流程")
        self._refresh_beautify_prompt_button()

    def _current_image_model_label(self) -> str:
        if not hasattr(self, "image_model_combo"):
            return DEFAULT_MODEL_LABEL
        return str(self.image_model_combo.currentData() or self.image_model_combo.currentText() or DEFAULT_MODEL_LABEL)

    def on_material_preset_changed(self) -> None:
        if not hasattr(self, "material_preset_combo"):
            return
        preset_name = str(self.material_preset_combo.currentData() or "").strip()
        if not preset_name:
            return
        preset = MATERIAL_PRESETS.get(preset_name)
        if preset is None:
            return
        self.feed_spin.setValue(int(preset["feed_rate"]))
        self.power_spin.setValue(int(preset["power"]))

    def _sync_material_preset_combo(self) -> None:
        if not hasattr(self, "material_preset_combo"):
            return
        current_feed = int(self.feed_spin.value())
        current_power = int(self.power_spin.value())
        matched_index = 0
        for index in range(1, self.material_preset_combo.count()):
            preset_name = str(self.material_preset_combo.itemData(index) or "").strip()
            preset = MATERIAL_PRESETS.get(preset_name)
            if preset is None:
                continue
            if preset["feed_rate"] == current_feed and preset["power"] == current_power:
                matched_index = index
                break
        self.material_preset_combo.blockSignals(True)
        self.material_preset_combo.setCurrentIndex(matched_index)
        self.material_preset_combo.blockSignals(False)

    def _refresh_beautify_prompt_button(self) -> None:
        if not hasattr(self, "btn_beautify_prompt"):
            return
        ai_generating = hasattr(self, "generate_image_button") and not self.generate_image_button.isEnabled()
        self.btn_beautify_prompt.setText(
            "✨ 正在施展魔法..." if self._is_prompt_optimizing else "✨ 一键美化提示词"
        )
        self.btn_beautify_prompt.setEnabled((not self._is_prompt_optimizing) and (not ai_generating))

    def _set_prompt_optimizing(self, optimizing: bool) -> None:
        self._is_prompt_optimizing = optimizing
        self._refresh_beautify_prompt_button()

    def _clear_prompt_optimizer_thread(self, thread: PromptOptimizerThread) -> None:
        if self.prompt_optimizer_thread is thread:
            self.prompt_optimizer_thread = None

    def on_beautify_prompt(self) -> None:
        prompt = self.prompt_text_edit.toPlainText().strip()
        if not prompt:
            QtWidgets.QMessageBox.warning(self, "提示", "请先输入需要美化的提示词")
            return
        self._set_prompt_optimizing(True)
        thread = PromptOptimizerThread(prompt=prompt, parent=self)
        self.prompt_optimizer_thread = thread
        thread.started_signal.connect(self.append_log)
        thread.finished_signal.connect(self.on_prompt_optimized)
        thread.error_signal.connect(self.on_prompt_optimize_error)
        thread.finished.connect(lambda: self._set_prompt_optimizing(False))
        thread.finished.connect(lambda: self._clear_prompt_optimizer_thread(thread))
        thread.finished.connect(thread.deleteLater)
        thread.start()

    def on_prompt_optimized(self, optimized_prompt: str) -> None:
        self._set_prompt_optimizing(False)
        self.prompt_text_edit.setPlainText(optimized_prompt)
        self.append_log("提示词美化完成，已自动回填到创意描述。")

    def on_prompt_optimize_error(self, message: str) -> None:
        self._set_prompt_optimizing(False)
        self.append_log(f"[错误] 提示词美化失败：{message}", color="#C53030", bold=True)
        QtWidgets.QMessageBox.warning(self, "提示词美化失败", message)

    def _current_contour_processing_params(self) -> Dict[str, int]:
        return {
            "detail_level": int(self.contour_detail_spin.value()) if hasattr(self, "contour_detail_spin") else 3,
            "denoise_level": int(self.contour_denoise_spin.value()) if hasattr(self, "contour_denoise_spin") else 2,
            "min_area": int(self.contour_min_area_spin.value()) if hasattr(self, "contour_min_area_spin") else 20,
            "smoothing_passes": int(self.contour_smooth_spin.value()) if hasattr(self, "contour_smooth_spin") else 2,
        }

    def _load_image_into_pipeline(
        self,
        image_path: str,
        source_name: str,
        preview_pixmap: Optional[QtGui.QPixmap] = None,
    ) -> None:
        try:
            contours, contours_path = process_image_to_contours(
                image_path,
                **self._current_contour_processing_params(),
            )
        except ImageProcessingError as exc:
            self._show_task_error("图像处理失败", str(exc))
            return
        except Exception as exc:
            self._show_task_error("图像处理失败", f"图像处理出现未知异常: {exc}")
            return

        self.current_image_path = image_path
        self.current_contours_path = contours_path
        self.current_contours = contours
        self.current_gcode_lines = []
        self.current_preview_lines = []
        if hasattr(self, "gcode_preview_widget"):
            self.gcode_preview_widget.clear_preview()
        if hasattr(self, "gcode_overlay_widget"):
            self.gcode_overlay_widget.clear_preview()
        if preview_pixmap is not None and not preview_pixmap.isNull():
            self.original_image_label.set_pixmap_image(preview_pixmap)
        else:
            self.original_image_label.set_image(image_path)
        self.contour_image_label.set_image(contours_path)
        self._refresh_preview_summary()
        self.append_log(f"{source_name}加载完成：{image_path}")
        self.append_log(f"轮廓提取完成，共 {len(contours)} 条路径，已完成降噪与居中排版。")
        saved_contour_path = self.task_manager.on_contour_ready(contours_path, len(contours))
        if saved_contour_path:
            self.current_contours_path = saved_contour_path
        if hasattr(self, "gcode_overlay_widget"):
            self.gcode_overlay_widget.set_background_image(self.current_contours_path)
        if hasattr(self, "result_preview_mode_combo"):
            index = self.result_preview_mode_combo.findData("contour")
            if index >= 0:
                self.result_preview_mode_combo.setCurrentIndex(index)

    def on_reprocess_current_image(self) -> None:
        if not self.current_image_path:
            QtWidgets.QMessageBox.warning(self, "提示", "请先生成或导入图片")
            return
        self.append_log(
            "正在按当前轮廓参数重新提取："
            f"细节 {self.contour_detail_spin.value()} / 降噪 {self.contour_denoise_spin.value()} / "
            f"最小面积 {self.contour_min_area_spin.value()} / 平滑 {self.contour_smooth_spin.value()}"
        )
        self._load_image_into_pipeline(self.current_image_path, "当前图片")

    def _generate_gcode_for_current_contours(self, announce_preview: bool = True) -> bool:
        if not self.current_contours:
            QtWidgets.QMessageBox.warning(self, "提示", "请先生成图像并提取轮廓")
            return False
        self._ensure_task_for_gcode()
        self._is_generating_gcode = True
        self._refresh_task_status_panel()
        try:
            self.current_gcode_lines = generate_gcode(
                self.current_contours,
                width_mm=self.width_spin.value(),
                height_mm=self.height_spin.value(),
                feed_rate=self.feed_spin.value(),
                power=self.power_spin.value(),
            )
        except Exception as exc:
            self._is_generating_gcode = False
            self._show_task_error("G-Code 生成失败", str(exc))
            return False
        finally:
            if self.task_manager.state == TaskState.GCODE_GENERATING:
                self._is_generating_gcode = False

        saved_gcode_path = self.task_manager.on_gcode_ready(self.current_gcode_lines)
        if hasattr(self, "gcode_preview_widget"):
            self.gcode_preview_widget.set_gcode_lines(self.current_gcode_lines)
        if hasattr(self, "gcode_overlay_widget"):
            self.gcode_overlay_widget.set_background_image(self.current_contours_path)
            self.gcode_overlay_widget.set_gcode_lines(self.current_gcode_lines)
        if hasattr(self, "result_preview_mode_combo"):
            index = self.result_preview_mode_combo.findData("overlay")
            if index >= 0:
                self.result_preview_mode_combo.setCurrentIndex(index)
        self._refresh_preview_summary()
        if announce_preview:
            preview = "\n".join(self.current_gcode_lines[:20])
            self.append_log("G-Code 生成完成，预览前 20 行：")
            self.append_log(
                f"当前任务尺寸: {self.width_spin.value():.1f} x {self.height_spin.value():.1f} mm，"
                "输出坐标保持第一象限正数，可直接按当前板卡逻辑使用。"
            )
            self.append_log(preview)
        else:
            self.append_log("比赛流程已自动完成 G-Code 生成。")
        if saved_gcode_path:
            self.append_log(f"G-Code 产物已归档：{saved_gcode_path}")
        return True

    def toggle_device_connection(self) -> None:
        if self.serial_thread.is_connected():
            self.serial_thread.disconnect_transport()
            self._update_connection_ui(False)
            self.append_log("设备连接已断开。")
            return

        if self._current_transport_mode() == "wifi":
            host = self.wifi_host_edit.text().strip()
            if not host:
                QtWidgets.QMessageBox.warning(self, "提示", "请输入发射板 WiFi IP 地址")
                return
            try:
                self.serial_thread.connect_wifi(host, int(self.wifi_port_spin.value()))
                self._update_connection_ui(True)
                self.append_log(f"WiFi 连接成功：{host}:{self.wifi_port_spin.value()}")
                self.left_tabs.setCurrentIndex(1)
            except Exception as exc:
                QtWidgets.QMessageBox.critical(self, "WiFi 错误", str(exc))
                self.append_log(f"[错误] {exc}")
            return

        port = str(self.port_combo.currentData() or "").strip()
        if not port:
            QtWidgets.QMessageBox.warning(self, "提示", "请先选择可用串口")
            return
        baud_rate = self._current_baud_rate()
        if baud_rate is None:
            return
        try:
            self.serial_thread.connect_port(port, baud_rate)
            self._update_connection_ui(True)
            self.append_log(f"串口连接成功：{port} @ {baud_rate}")
            self.left_tabs.setCurrentIndex(1)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "串口错误", str(exc))
            self.append_log(f"[错误] {exc}")

    def on_generate_image(self) -> None:
        self._start_ai_generation(auto_generate=False, auto_send=False)

    def _start_ai_generation(self, auto_generate: bool, auto_send: bool) -> None:
        prompt = self.prompt_text_edit.toPlainText().strip()
        if not prompt:
            QtWidgets.QMessageBox.warning(self, "提示", "请输入 AI 提示词")
            return
        if self._is_prompt_optimizing:
            QtWidgets.QMessageBox.information(self, "提示", "提示词正在美化中，请等待完成后再开始生图。")
            return

        model_label = self._current_image_model_label()
        self.current_source_type = "ai"
        self._begin_task("ai")
        self._pending_auto_generate = auto_generate
        self._pending_auto_send = auto_send
        self._set_ai_generating(True)
        self.append_log(f"当前生图模型：{model_label}")
        self.ai_thread = AIGeneratorThread(prompt=prompt, model_label=model_label, parent=self)
        self.ai_thread.started_signal.connect(self.append_log)
        self.ai_thread.success_signal.connect(self.on_ai_image_generated)
        self.ai_thread.error_signal.connect(self.on_ai_image_error)
        self.ai_thread.finished.connect(lambda: self._set_ai_generating(False))
        self.ai_thread.finished.connect(self.ai_thread.deleteLater)
        self.ai_thread.start()

    def on_start_competition_flow(self) -> None:
        if not self._ensure_device_connected("一键比赛流程"):
            return
        if not self._confirm_device_job("一键比赛流程"):
            return
        self.append_log(
            f"比赛流程启动：模型={self._current_image_model_label()}，设备={self.serial_thread.connection_description()}。"
        )
        self._start_ai_generation(auto_generate=True, auto_send=True)

    def on_import_local_image(self) -> None:
        image_path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self,
            "选择本地图片",
            str(Path.cwd()),
            "Image Files (*.png *.jpg *.jpeg *.bmp);;All Files (*)",
        )
        if not image_path:
            return
        self.current_source_type = "local"
        self._begin_task("local")
        task_image_path = self.task_manager.on_local_image_loaded(image_path)
        self._load_image_into_pipeline(task_image_path, "本地图片")

    def on_ai_image_generated(self, result: object) -> None:
        auto_generate = self._pending_auto_generate
        auto_send = self._pending_auto_send
        self._pending_auto_generate = False
        self._pending_auto_send = False
        preview_pixmap: Optional[QtGui.QPixmap] = None

        if isinstance(result, GeneratedImageResult):
            image_path = result.image_path
            pixmap = QtGui.QPixmap()
            if pixmap.loadFromData(result.image_bytes):
                preview_pixmap = pixmap
        else:
            image_path = str(result)

        self.append_log(f"AI 图像生成完成：{image_path}")
        task_image_path = self.task_manager.on_ai_image_ready(image_path)
        self._load_image_into_pipeline(task_image_path, "AI 图像", preview_pixmap=preview_pixmap)
        if not auto_generate:
            return
        if not self._generate_gcode_for_current_contours(announce_preview=False):
            return
        if not auto_send:
            return
        if not self._ensure_device_connected("比赛流程自动发送"):
            return
        try:
            self.serial_thread.enqueue_gcode(self.current_gcode_lines)
            self._active_device_job = "task"
            self.task_manager.on_sending_started(len(self.current_gcode_lines))
            self.append_log("比赛流程已自动下发任务。")
        except Exception as exc:
            self._active_device_job = ""
            self._show_task_error("任务发送失败", str(exc))

    def on_ai_image_error(self, message: str) -> None:
        self._pending_auto_generate = False
        self._pending_auto_send = False
        self._show_task_error("生成失败", message)

    def on_generate_gcode(self) -> None:
        self._generate_gcode_for_current_contours(announce_preview=True)

    def on_preview_bbox(self) -> None:
        if not self.current_contours:
            QtWidgets.QMessageBox.warning(self, "提示", "请先生成图像并提取轮廓")
            return
        if not self._ensure_device_connected("红光边框预览"):
            return
        try:
            self.current_preview_lines = generate_preview_gcode(
                self.current_contours,
                width_mm=self.width_spin.value(),
                height_mm=self.height_spin.value(),
            )
        except Exception as exc:
            self._show_task_error("预览 G-Code 生成失败", str(exc), mark_failed=False)
            return

        try:
            self.serial_thread.enqueue_gcode(self.current_preview_lines)
            self._active_device_job = "preview"
            self.append_log("已下发红光边框预览任务，预览路径同样使用工作区正坐标。")
        except Exception as exc:
            self._active_device_job = ""
            self._show_task_error("任务发送失败", str(exc), mark_failed=False)

    def on_start_engraving(self) -> None:
        if not self.current_gcode_lines:
            self.on_generate_gcode()
            if not self.current_gcode_lines:
                return
        if not self._ensure_device_connected("激光雕刻"):
            return
        if not self._confirm_device_job("激光雕刻", line_count=len(self.current_gcode_lines)):
            return
        self._ensure_task_for_sending()
        try:
            self.serial_thread.enqueue_gcode(self.current_gcode_lines)
            self._active_device_job = "task"
            self.task_manager.on_sending_started(len(self.current_gcode_lines))
            self.append_log("已下发雕刻任务。")
        except Exception as exc:
            self._active_device_job = ""
            self._show_task_error("任务发送失败", str(exc))

    def on_export_gcode(self) -> None:
        if not self.current_gcode_lines:
            self.on_generate_gcode()
            if not self.current_gcode_lines:
                return

        default_path = str(Path.cwd() / "ai_output.gcode")
        output_path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self,
            "导出 G-Code",
            default_path,
            "G-Code Files (*.gcode *.nc *.txt);;All Files (*)",
        )
        if not output_path:
            return

        try:
            saved = save_gcode_file(self.current_gcode_lines, output_path)
            self.append_log(f"G-Code 已导出：{saved}")
            self.append_log("如果现场不直接走应用内串口或 WiFi，可把该文件导入 LaserGRBL 作为后备方案。")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "导出失败", str(exc))
            self.append_log(f"[错误] {exc}")

    def on_emergency_stop(self) -> None:
        if not self.serial_thread.is_connected():
            self.append_log("[提示] 当前未连接设备，急停命令未发送。")
            return
        if self._active_device_job == "task":
            self.task_manager.note_stop_reason("操作员手动触发急停，正在等待设备停止。")
        self.serial_thread.emergency_stop()
        self.append_log("已触发急停。")

    def on_job_finished(self, success: bool, message: str) -> None:
        active_device_job = self._active_device_job
        self._active_device_job = ""
        if success:
            if active_device_job == "task":
                self.task_manager.on_task_completed()
            self.append_log(f"[完成] {message}")
            if active_device_job == "task":
                self._archive_task_to_history()
        else:
            if active_device_job == "task":
                self._show_task_error("任务状态", message)
                return
            self.append_log(f"[失败] {message}", color="#C53030", bold=True)
            QtWidgets.QMessageBox.warning(self, "任务状态", message)
