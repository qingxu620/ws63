"""
主界面模块。

本版面向教育和创客场景，采用明亮友好的卡片式布局：
1. 左侧为分步标签页式控制面板，按“连接 -> 创作 -> 雕刻”组织工作流；
2. 右侧为展示区，上方看图、下方看消息；
3. 保留 AI 生图、本地图导入、G-Code 生成、设备发送的完整闭环；
4. 比赛版可在同一界面切换“串口 UART”或“WiFi TCP”两种下发方式。
"""

from __future__ import annotations

from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .ai_image_generator import AIGeneratorThread
from .gcode_generator import (
    BOARD_WORK_AREA_X_MM,
    BOARD_WORK_AREA_Y_MM,
    generate_gcode,
    generate_preview_gcode,
    save_gcode_file,
)
from .image_processing import ImageProcessingError, NormalizedPath, process_image_to_contours
from .qt_compat import QT_API, QtCore, QtGui, QtWidgets
from .serial_worker import SerialWorkerThread


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

QLineEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox {
    background-color: #F7FAFC;
    border: 1px solid #E2E8F0;
    border-radius: 8px;
    padding: 8px;
    color: #2D3748;
}
QLineEdit:focus, QTextEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border: 2px solid #667EEA;
    background-color: #FFFFFF;
}

QTextEdit#LogBox {
    background-color: #FFFFFF;
    border: 1px solid #E4E7EB;
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
    background-color: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 14px;
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
    background-color: #FFFFFF;
    border: 1px dashed #CBD5E0;
    border-radius: 12px;
    color: #A0AEC0;
    font-weight: normal;
}

QProgressBar {
    border: none;
    background-color: #E2E8F0;
    border-radius: 10px;
    text-align: center;
    color: #2D3748;
    font-weight: bold;
    font-size: 13px;
    min-height: 24px;
    max-height: 28px;
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
    padding: 10px 12px;
    margin-right: 4px;
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

PROMPT_TEMPLATES = [
    (
        "创客徽章",
        "适合作品展示的圆形徽章，强调芯片、无线连接与激光雕刻元素。",
        "设计一个创客徽章图案，主体包含芯片、电路线、激光光束和简洁机械元素，"
        "线稿插画风格，主体居中，白底，高对比，适合激光雕刻轮廓提取",
    ),
    (
        "校园吉祥物",
        "适合演示 AI 生图能力，兼顾可爱感和轮廓稳定性。",
        "画一个校园创客吉祥物，小动物戴护目镜并拿着小型电路板，表情自信，"
        "卡通线稿风格，主体完整，背景极简，适合激光雕刻",
    ),
    (
        "工业铭牌",
        "适合展示工程产品感，图形规整、容易稳定出 G-Code。",
        "设计一个工业设备铭牌图案，包含几何边框、型号标记区、芯片和激光符号，"
        "简洁硬朗的技术线稿风格，布局规整，白底高对比，适合激光雕刻",
    ),
]

LIVE_STATE_DISPLAY: Dict[str, str] = {
    "Idle": "空闲",
    "Run": "运行中",
    "Hold": "暂停",
    "Alarm": "报警",
}


class ImageLabel(QtWidgets.QLabel):
    """用于显示图片的白底预览画布。"""

    def __init__(self, placeholder: str, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self._pixmap: Optional[QtGui.QPixmap] = None
        self.setObjectName("PreviewCanvas")
        self.setMinimumSize(180, 140)
        self.setAlignment(QtCore.Qt.AlignCenter)
        self.setWordWrap(True)
        self.setText(placeholder)
        self.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)

    def set_image(self, image_path: str) -> None:
        pixmap = QtGui.QPixmap(image_path)
        if pixmap.isNull():
            self._pixmap = None
            self.setText(f"图片加载失败\n{image_path}")
            return
        self._pixmap = pixmap
        self._refresh_pixmap()

    def clear_to_placeholder(self, text: str) -> None:
        self._pixmap = None
        self.setPixmap(QtGui.QPixmap())
        self.setText(text)

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

        self.current_image_path: Optional[str] = None
        self.current_contours_path: Optional[str] = None
        self.current_contours: List[NormalizedPath] = []
        self.current_gcode_lines: List[str] = []
        self.current_preview_lines: List[str] = []
        self.ai_thread: Optional[AIGeneratorThread] = None
        self._pending_auto_generate = False
        self._pending_auto_send = False

        self.serial_thread = SerialWorkerThread(self)
        self.serial_thread.log_signal.connect(self.append_log)
        self.serial_thread.status_signal.connect(self.append_log)
        self.serial_thread.progress_signal.connect(self.on_progress_changed)
        self.serial_thread.ports_signal.connect(self.update_ports)
        self.serial_thread.telemetry_signal.connect(self.on_live_status_changed)
        self.serial_thread.finished_signal.connect(self.on_job_finished)
        self.serial_thread.start()

        self._build_ui()
        self.apply_stylesheet()
        self._update_connection_ui(False)
        self.reset_live_status_strip()
        QtCore.QTimer.singleShot(0, self._position_preview_floating_bar)
        self.refresh_ports()
        self.append_log("欢迎使用 AI 智能创作中枢，准备开始创作吧。")
        self.append_log(
            f"当前板卡工作区: X 0~{BOARD_WORK_AREA_X_MM:.1f} mm / Y 0~{BOARD_WORK_AREA_Y_MM:.1f} mm，"
            "原点按 LaserGRBL 习惯位于工作区左下角。"
        )

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # pragma: no cover - GUI 事件
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
        root_layout.addWidget(self.horizontal_splitter)

        left_panel = self._build_left_panel()
        right_panel = self._build_right_panel()

        self.horizontal_splitter.addWidget(left_panel)
        self.horizontal_splitter.addWidget(right_panel)
        self.horizontal_splitter.setStretchFactor(0, 0)
        self.horizontal_splitter.setStretchFactor(1, 1)
        self.horizontal_splitter.setSizes([360, 1080])

    def _build_left_panel(self) -> QtWidgets.QWidget:
        self.left_panel = QtWidgets.QWidget()
        self.left_panel.setMinimumWidth(350)
        self.left_panel.setMaximumWidth(400)
        self.left_panel.setSizePolicy(QtWidgets.QSizePolicy.Fixed, QtWidgets.QSizePolicy.Expanding)

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
        machine_page = self._build_machine_scroll_page()

        self.left_tabs.addTab(serial_page, "连接")
        self.left_tabs.addTab(ai_page, "创作")
        self.left_tabs.addTab(machine_page, "雕刻")
        self.left_tabs.setTabToolTip(0, "步骤一：连接设备")
        self.left_tabs.setTabToolTip(1, "步骤二：AI 创作与导入")
        self.left_tabs.setTabToolTip(2, "步骤三：雕刻执行")
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
            self._build_action_card(),
        )

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
        layout.setContentsMargins(16, 10, 16, 10)
        layout.setSpacing(6)

        # -- Row 1: title + progress bar --
        header_row = QtWidgets.QHBoxLayout()
        header_row.setSpacing(16)

        title_label = QtWidgets.QLabel("\U0001f4e1 \u72b6\u6001\u4e0e\u8fdb\u5ea6")
        title_label.setObjectName("CardTitle")
        title_label.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Preferred)
        header_row.addWidget(title_label)

        self.progress_bar = QtWidgets.QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setFormat("\u4efb\u52a1\u8fdb\u5ea6 %p%")
        self.progress_bar.setFixedHeight(24)
        self.progress_bar.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        header_row.addWidget(self.progress_bar, 1)

        layout.addLayout(header_row)

        # -- Row 2: flat telemetry strip (single row, 6 columns) --
        telemetry_strip = QtWidgets.QFrame()
        telemetry_strip.setObjectName("LiveTelemetryStrip")
        telemetry_strip.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        grid = QtWidgets.QGridLayout(telemetry_strip)
        grid.setContentsMargins(12, 8, 12, 8)
        grid.setHorizontalSpacing(6)
        grid.setVerticalSpacing(2)

        self.live_state_value = self._create_live_telemetry_pair(grid, 0, 0, "\u8bbe\u5907\u72b6\u6001")
        self.live_x_value = self._create_live_telemetry_pair(grid, 0, 1, "X")
        self.live_y_value = self._create_live_telemetry_pair(grid, 0, 2, "Y")
        self.live_feed_value = self._create_live_telemetry_pair(grid, 0, 3, "F")
        self.live_power_value = self._create_live_telemetry_pair(grid, 0, 4, "S")
        self.live_endpoint_value = self._create_live_telemetry_pair(grid, 0, 5, "\u76ee\u6807")

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

        self.port_combo = ClickToSelectComboBox()
        self.refresh_ports_button = QtWidgets.QPushButton("刷新串口")
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
        port_row.addWidget(self.port_combo, 1)
        port_row.addWidget(self.refresh_ports_button)
        serial_layout.addLayout(port_row)
        serial_layout.addWidget(QtWidgets.QLabel("波特率"))
        serial_layout.addWidget(self.baud_combo)
        layout.addWidget(self.serial_config_widget)

        self.wifi_host_edit = QtWidgets.QLineEdit("192.168.43.1")
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

        self.template_combo = ClickToSelectComboBox()
        for name, description, prompt in PROMPT_TEMPLATES:
            self.template_combo.addItem(name, {"description": description, "prompt": prompt})
        self.template_combo.currentIndexChanged.connect(self.on_prompt_template_changed)
        self._apply_min_height(self.template_combo)
        layout.addWidget(QtWidgets.QLabel("创作模板"))
        layout.addWidget(self.template_combo)

        self.template_hint_label = QtWidgets.QLabel()
        self.template_hint_label.setWordWrap(True)
        self.template_hint_label.setStyleSheet("font-size:12px; font-weight:normal; color:#718096;")
        layout.addWidget(self.template_hint_label)

        self.apply_template_button = QtWidgets.QPushButton("填入模板提示词")
        self.apply_template_button.clicked.connect(self.apply_selected_template)
        self._apply_min_height(self.apply_template_button)
        layout.addWidget(self.apply_template_button)

        layout.addWidget(QtWidgets.QLabel("创意描述"))
        self.prompt_edit = QtWidgets.QTextEdit()
        self.prompt_edit.setObjectName("PromptBox")
        self.prompt_edit.setPlaceholderText("例如：画一只戴着护目镜的创客小猫，线稿风格，适合激光雕刻")
        self.prompt_edit.setMinimumHeight(120)
        self.prompt_edit.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        layout.addWidget(self.prompt_edit)

        self.generate_image_button = QtWidgets.QPushButton("✨ 魔法生成图像")
        self.generate_image_button.setObjectName("BtnAI")
        self.generate_image_button.clicked.connect(self.on_generate_image)
        self._apply_min_height(self.generate_image_button)
        layout.addWidget(self.generate_image_button)

        self.competition_flow_button = QtWidgets.QPushButton("⚡ 一键比赛流程")
        self.competition_flow_button.setObjectName("BtnStart")
        self.competition_flow_button.clicked.connect(self.on_start_competition_flow)
        self._apply_min_height(self.competition_flow_button)
        layout.addWidget(self.competition_flow_button)

        self.import_image_button = QtWidgets.QPushButton("📁 导入本地图片")
        self.import_image_button.setObjectName("BtnLocal")
        self.import_image_button.clicked.connect(self.on_import_local_image)
        self._apply_min_height(self.import_image_button)
        layout.addWidget(self.import_image_button)

        self.competition_hint_label = QtWidgets.QLabel(
            "比赛流程会自动执行：AI 生图 -> 轮廓提取 -> G-Code 生成 -> 发送到当前设备。"
        )
        self.competition_hint_label.setWordWrap(True)
        self.competition_hint_label.setStyleSheet("font-size:12px; font-weight:normal; color:#718096;")
        layout.addWidget(self.competition_hint_label)

        self.on_prompt_template_changed()
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

        self.feed_spin = ClickToEditSpinBox()
        self.feed_spin.setRange(1, 100000)
        self.feed_spin.setValue(6000)

        self.power_spin = ClickToEditSpinBox()
        self.power_spin.setRange(0, 1000)
        self.power_spin.setValue(200)

        self._apply_min_height(self.width_spin, self.height_spin, self.feed_spin, self.power_spin)

        form.addRow("宽度", self.width_spin)
        form.addRow("高度", self.height_spin)
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
        right_preview = self._create_preview_panel("轮廓", "等待轮廓提取结果")

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
        if original_ready and contour_ready:
            summary = "原图与轮廓已就绪"
        elif original_ready:
            summary = "原图已就绪，待轮廓"
        else:
            summary = "等待导入或生成"
        self.preview_compact_summary.setText(summary)

    def _create_live_telemetry_pair(
        self,
        layout: QtWidgets.QGridLayout,
        row: int,
        column: int,
        title: str,
    ) -> QtWidgets.QLabel:
        title_label = QtWidgets.QLabel(title)
        title_label.setObjectName("LiveTelemetryLabel")
        title_label.setAlignment(QtCore.Qt.AlignCenter)
        value_label = QtWidgets.QLabel("--")
        value_label.setObjectName("LiveTelemetryValue")
        value_label.setAlignment(QtCore.Qt.AlignCenter)
        value_label.setWordWrap(False)
        layout.addWidget(title_label, row * 2, column)
        layout.addWidget(value_label, row * 2 + 1, column)
        layout.setColumnStretch(column, 1)
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

    def _build_log_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("📝 系统消息板", "这里会显示串口或 WiFi 收发、G-Code 预览和任务运行状态，便于比赛演示。")
        card.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        card.setMinimumHeight(180)

        self.log_edit = QtWidgets.QTextEdit()
        self.log_edit.setReadOnly(True)
        self.log_edit.setObjectName("LogBox")
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

    def _update_connection_ui(self, connected: bool) -> None:
        """根据设备连接状态更新相关按钮可用性，避免误操作。"""

        mode = self._current_transport_mode()
        if hasattr(self, "preview_button"):
            self.preview_button.setEnabled(connected)
        if hasattr(self, "start_button"):
            self.start_button.setEnabled(connected)
        if hasattr(self, "emergency_button"):
            self.emergency_button.setEnabled(connected)
        self.transport_combo.setEnabled(not connected)
        self.serial_config_widget.setVisible(mode == "serial")
        self.wifi_config_widget.setVisible(mode == "wifi")
        self.port_combo.setEnabled((mode == "serial") and (not connected))
        self.baud_combo.setEnabled((mode == "serial") and (not connected))
        self.refresh_ports_button.setEnabled((mode == "serial") and (not connected))
        self.wifi_host_edit.setEnabled((mode == "wifi") and (not connected))
        self.wifi_port_spin.setEnabled((mode == "wifi") and (not connected))
        if connected:
            self.connect_button.setText("断开设备")
            self.connection_summary_label.setText(
                f"当前已通过{'WiFi TCP' if mode == 'wifi' else '串口 UART'}连接：{self.serial_thread.connection_description()}"
            )
            self.live_state_value.setText("已连接")
            self.live_endpoint_value.setText(self.serial_thread.connection_description() or "--")
        else:
            self.connect_button.setText("连接设备")
            if mode == "wifi":
                self.connection_summary_label.setText("当前选择 WiFi TCP，可直连发射板 SoftAP 或 STA 地址。")
            else:
                self.connection_summary_label.setText("当前选择串口 UART，可连接发射板业务串口。")
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

    def on_transport_mode_changed(self) -> None:
        self.serial_thread.set_transport_mode(self._current_transport_mode())
        self._update_connection_ui(self.serial_thread.is_connected())

    def reset_live_status_strip(self) -> None:
        if not hasattr(self, "live_state_value"):
            return
        self.live_state_value.setText("未连接")
        self.live_x_value.setText("--")
        self.live_y_value.setText("--")
        self.live_feed_value.setText("--")
        self.live_power_value.setText("--")
        self.live_endpoint_value.setText("--")

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

        self.live_state_value.setText(state_text)
        self.live_x_value.setText(f"{x:.3f}")
        self.live_y_value.setText(f"{y:.3f}")
        self.live_feed_value.setText(f"{feed:.0f}")
        self.live_power_value.setText(f"{power:.0f}")
        self.live_endpoint_value.setText(endpoint)

    def append_log(self, message: str) -> None:
        cursor = self.log_edit.textCursor()
        cursor.movePosition(QtGui.QTextCursor.End)
        cursor.insertText(message + "\n")
        self.log_edit.setTextCursor(cursor)
        self.log_edit.ensureCursorVisible()

    def on_progress_changed(self, value: int) -> None:
        self.progress_bar.setValue(value)

    def update_ports(self, ports: List[str]) -> None:
        current = self.port_combo.currentText()
        self.port_combo.blockSignals(True)
        self.port_combo.clear()
        self.port_combo.addItems(ports)
        if current and current in ports:
            self.port_combo.setCurrentText(current)
        self.port_combo.blockSignals(False)

    def refresh_ports(self) -> None:
        self.update_ports(self.serial_thread.list_available_ports())
        self.append_log("已刷新串口列表。")

    def _set_ai_generating(self, generating: bool) -> None:
        self.generate_image_button.setDisabled(generating)
        self.competition_flow_button.setDisabled(generating)
        self.apply_template_button.setDisabled(generating)
        self.template_combo.setDisabled(generating)
        self.import_image_button.setDisabled(generating)
        if generating:
            self.generate_image_button.setText("✨ 生成中...")
            self.competition_flow_button.setText("⚡ 比赛流程执行中...")
        else:
            self.generate_image_button.setText("✨ 魔法生成图像")
            self.competition_flow_button.setText("⚡ 一键比赛流程")

    def _current_template_payload(self) -> dict:
        payload = self.template_combo.currentData()
        return payload if isinstance(payload, dict) else {}

    def on_prompt_template_changed(self) -> None:
        payload = self._current_template_payload()
        description = payload.get("description", "选择一套更适合当前创作方向和轮廓提取的模板。")
        self.template_hint_label.setText(f"模板说明：{description}")

    def apply_selected_template(self) -> None:
        payload = self._current_template_payload()
        prompt = str(payload.get("prompt", "")).strip()
        if not prompt:
            return
        self.prompt_edit.setPlainText(prompt)
        self.append_log(f"已载入模板：{self.template_combo.currentText()}")

    def _load_image_into_pipeline(self, image_path: str, source_name: str) -> None:
        try:
            contours, contours_path = process_image_to_contours(image_path)
        except ImageProcessingError as exc:
            QtWidgets.QMessageBox.critical(self, "图像处理失败", str(exc))
            self.append_log(f"[错误] {exc}")
            return
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "未知错误", str(exc))
            self.append_log(f"[错误] 图像处理出现未知异常: {exc}")
            return

        self.current_image_path = image_path
        self.current_contours_path = contours_path
        self.current_contours = contours
        self.current_gcode_lines = []
        self.current_preview_lines = []
        self.original_image_label.set_image(image_path)
        self.contour_image_label.set_image(contours_path)
        self._refresh_preview_summary()
        self.append_log(f"{source_name}加载完成：{image_path}")
        self.append_log(f"轮廓提取完成，共 {len(contours)} 条路径，已完成降噪与居中排版。")

    def _generate_gcode_for_current_contours(self, announce_preview: bool = True) -> bool:
        if not self.current_contours:
            QtWidgets.QMessageBox.warning(self, "提示", "请先生成图像并提取轮廓")
            return False
        try:
            self.current_gcode_lines = generate_gcode(
                self.current_contours,
                width_mm=self.width_spin.value(),
                height_mm=self.height_spin.value(),
                feed_rate=self.feed_spin.value(),
                power=self.power_spin.value(),
            )
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "G-Code 生成失败", str(exc))
            self.append_log(f"[错误] {exc}")
            return False

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
            except Exception as exc:
                QtWidgets.QMessageBox.critical(self, "WiFi 错误", str(exc))
                self.append_log(f"[错误] {exc}")
            return

        port = self.port_combo.currentText().strip()
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
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "串口错误", str(exc))
            self.append_log(f"[错误] {exc}")

    def on_generate_image(self) -> None:
        self._start_ai_generation(auto_generate=False, auto_send=False)

    def _start_ai_generation(self, auto_generate: bool, auto_send: bool) -> None:
        prompt = self.prompt_edit.toPlainText().strip()
        if not prompt:
            QtWidgets.QMessageBox.warning(self, "提示", "请输入 AI 提示词")
            return

        self._pending_auto_generate = auto_generate
        self._pending_auto_send = auto_send
        self._set_ai_generating(True)
        self.ai_thread = AIGeneratorThread(prompt=prompt, parent=self)
        self.ai_thread.started_signal.connect(self.append_log)
        self.ai_thread.success_signal.connect(self.on_ai_image_generated)
        self.ai_thread.error_signal.connect(self.on_ai_image_error)
        self.ai_thread.finished.connect(lambda: self._set_ai_generating(False))
        self.ai_thread.finished.connect(self.ai_thread.deleteLater)
        self.ai_thread.start()

    def on_start_competition_flow(self) -> None:
        if not self._ensure_device_connected("一键比赛流程"):
            return
        self.append_log(
            f"比赛流程启动：模板={self.template_combo.currentText()}，设备={self.serial_thread.connection_description()}。"
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
        self._load_image_into_pipeline(image_path, "本地图片")

    def on_ai_image_generated(self, image_path: str) -> None:
        auto_generate = self._pending_auto_generate
        auto_send = self._pending_auto_send
        self._pending_auto_generate = False
        self._pending_auto_send = False
        self.append_log(f"AI 图像生成完成：{image_path}")
        self._load_image_into_pipeline(image_path, "AI 图像")
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
            self.append_log("比赛流程已自动下发任务。")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "任务发送失败", str(exc))
            self.append_log(f"[错误] {exc}")

    def on_ai_image_error(self, message: str) -> None:
        self._pending_auto_generate = False
        self._pending_auto_send = False
        QtWidgets.QMessageBox.critical(self, "生成失败", message)
        self.append_log(f"[错误] {message}")

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
            QtWidgets.QMessageBox.critical(self, "预览 G-Code 生成失败", str(exc))
            self.append_log(f"[错误] {exc}")
            return

        try:
            self.serial_thread.enqueue_gcode(self.current_preview_lines)
            self.append_log("已下发红光边框预览任务，预览路径同样使用工作区正坐标。")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "任务发送失败", str(exc))
            self.append_log(f"[错误] {exc}")

    def on_start_engraving(self) -> None:
        if not self.current_gcode_lines:
            self.on_generate_gcode()
            if not self.current_gcode_lines:
                return
        if not self._ensure_device_connected("激光雕刻"):
            return
        try:
            self.serial_thread.enqueue_gcode(self.current_gcode_lines)
            self.append_log("已下发雕刻任务。")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "任务发送失败", str(exc))
            self.append_log(f"[错误] {exc}")

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
        self.serial_thread.emergency_stop()
        self.append_log("已触发急停。")

    def on_job_finished(self, success: bool, message: str) -> None:
        if success:
            self.append_log(f"[完成] {message}")
        else:
            self.append_log(f"[失败] {message}")
            QtWidgets.QMessageBox.warning(self, "任务状态", message)
