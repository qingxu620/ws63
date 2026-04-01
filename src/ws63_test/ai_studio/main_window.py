"""
主界面模块。

本版面向教育和创客场景，采用明亮友好的卡片式布局：
1. 左侧为控制面板，放入 QScrollArea，窗口缩放时不再挤压变形；
2. 右侧为展示区，上方看图、下方看消息；
3. 保留 AI 生图、本地图导入、G-Code 生成、串口发送的完整闭环。
"""

from __future__ import annotations

from pathlib import Path
from typing import List, Optional, Tuple

from .ai_image_generator import AIGeneratorThread
from .gcode_generator import generate_gcode, generate_preview_gcode, save_gcode_file
from .image_processing import ImageProcessingError, NormalizedPath, process_image_to_contours
from .qt_compat import QT_API, QtCore, QtGui, QtWidgets
from .serial_worker import SerialWorkerThread


BRIGHT_MAKER_QSS = """
QWidget {
    background-color: #F0F4F8;
    color: #2D3748;
    font-family: "Microsoft YaHei", "PingFang SC", sans-serif;
    font-size: 14px;
}
QFrame#Card {
    background-color: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 16px;
    margin: 4px;
}
QLabel { background-color: transparent; font-weight: bold; color: #4A5568; }

QPushButton {
    background-color: #FFFFFF;
    border: 2px solid #E2E8F0;
    border-radius: 10px;
    padding: 10px 16px;
    font-weight: bold;
    color: #4A5568;
}
QPushButton:hover { border-color: #A0AEC0; background-color: #F7FAFC; }
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
    font-size: 12px;
    font-weight: normal;
    color: #718096;
    padding-bottom: 4px;
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
    border-radius: 8px;
    text-align: center;
    color: #2D3748;
    font-weight: bold;
}
QProgressBar::chunk {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #48BB78, stop:1 #81E6D9);
    border-radius: 8px;
}

QSplitter::handle {
    background-color: #DDE6F1;
}
"""


class ImageLabel(QtWidgets.QLabel):
    """用于显示图片的白底预览画布。"""

    def __init__(self, placeholder: str, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self._pixmap: Optional[QtGui.QPixmap] = None
        self.setObjectName("PreviewCanvas")
        self.setMinimumSize(280, 250)
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

        self.serial_thread = SerialWorkerThread(self)
        self.serial_thread.log_signal.connect(self.append_log)
        self.serial_thread.status_signal.connect(self.append_log)
        self.serial_thread.progress_signal.connect(self.on_progress_changed)
        self.serial_thread.ports_signal.connect(self.update_ports)
        self.serial_thread.finished_signal.connect(self.on_job_finished)
        self.serial_thread.start()

        self._build_ui()
        self.apply_stylesheet()
        self.refresh_ports()
        self.append_log("欢迎使用 AI 智能创作中枢，准备开始创作吧。")

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # pragma: no cover - GUI 事件
        self.serial_thread.stop()
        super().closeEvent(event)

    def _build_ui(self) -> None:
        central = QtWidgets.QWidget()
        self.setCentralWidget(central)

        root_layout = QtWidgets.QHBoxLayout(central)
        root_layout.setContentsMargins(12, 12, 12, 12)
        root_layout.setSpacing(0)

        self.horizontal_splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        root_layout.addWidget(self.horizontal_splitter)

        left_scroll = self._build_left_scroll_area()
        right_panel = self._build_right_panel()

        self.horizontal_splitter.addWidget(left_scroll)
        self.horizontal_splitter.addWidget(right_panel)
        self.horizontal_splitter.setStretchFactor(0, 0)
        self.horizontal_splitter.setStretchFactor(1, 1)
        self.horizontal_splitter.setSizes([360, 1080])

    def _build_left_scroll_area(self) -> QtWidgets.QScrollArea:
        self.left_scroll = QtWidgets.QScrollArea()
        self.left_scroll.setWidgetResizable(True)
        self.left_scroll.setMinimumWidth(350)
        self.left_scroll.setMaximumWidth(400)
        self.left_scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        self.left_scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        self.left_scroll.setStyleSheet("QScrollArea { border: none; background-color: transparent; }")
        self.left_scroll.setSizePolicy(QtWidgets.QSizePolicy.Fixed, QtWidgets.QSizePolicy.Expanding)

        self.left_container = QtWidgets.QWidget()
        self.left_container.setSizePolicy(QtWidgets.QSizePolicy.Preferred, QtWidgets.QSizePolicy.Maximum)
        self.left_layout = QtWidgets.QVBoxLayout(self.left_container)
        self.left_layout.setContentsMargins(0, 0, 0, 0)
        self.left_layout.setSpacing(8)

        title = QtWidgets.QLabel("✨ AI 智能创作中枢")
        title.setStyleSheet("font-size:22px; font-weight:800; color:#2C3E50; padding: 6px 12px;")
        self.left_layout.addWidget(title)

        subtitle = QtWidgets.QLabel("让学生从创意出发，一步完成图像生成、轮廓提取与激光雕刻。")
        subtitle.setWordWrap(True)
        subtitle.setStyleSheet("font-size:13px; color:#7F8C8D; font-weight:normal; padding: 0 12px 8px 12px;")
        self.left_layout.addWidget(subtitle)

        self.left_layout.addWidget(self._build_serial_card())
        self.left_layout.addWidget(self._build_ai_card())
        self.left_layout.addWidget(self._build_params_card())
        self.left_layout.addWidget(self._build_action_card())
        self.left_layout.addStretch(1)

        self.left_scroll.setWidget(self.left_container)
        return self.left_scroll

    def _build_right_panel(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(panel)
        layout.setContentsMargins(0, 0, 0, 0)

        right_splitter = QtWidgets.QSplitter(QtCore.Qt.Vertical)
        preview_card = self._build_preview_card()
        log_card = self._build_log_card()
        right_splitter.addWidget(preview_card)
        right_splitter.addWidget(log_card)
        right_splitter.setStretchFactor(0, 3)
        right_splitter.setStretchFactor(1, 2)
        right_splitter.setSizes([560, 260])

        layout.addWidget(right_splitter)
        return panel

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

    def _build_serial_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("🔌 串口连接", "连接发射板业务串口，默认使用 115200 波特率。")

        self.port_combo = QtWidgets.QComboBox()
        self.refresh_ports_button = QtWidgets.QPushButton("刷新串口")
        self.refresh_ports_button.clicked.connect(self.refresh_ports)

        self._apply_min_height(self.port_combo, self.refresh_ports_button)

        port_label = QtWidgets.QLabel("可用串口")
        layout.addWidget(port_label)
        port_row = QtWidgets.QHBoxLayout()
        port_row.setSpacing(8)
        port_row.addWidget(self.port_combo, 1)
        port_row.addWidget(self.refresh_ports_button)
        layout.addLayout(port_row)

        self.baud_combo = QtWidgets.QComboBox()
        self.baud_combo.addItems(["115200", "230400", "460800", "921600"])
        self.baud_combo.setCurrentText("115200")
        self._apply_min_height(self.baud_combo)
        layout.addWidget(QtWidgets.QLabel("波特率"))
        layout.addWidget(self.baud_combo)

        self.connect_button = QtWidgets.QPushButton("连接串口")
        self.connect_button.clicked.connect(self.toggle_serial_connection)
        self._apply_min_height(self.connect_button)
        layout.addWidget(self.connect_button)
        return card

    def _build_ai_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("🎨 AI 创作区", "输入提示词生成图像，或直接导入本地图片进行雕刻。")

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

        self.import_image_button = QtWidgets.QPushButton("📁 导入本地图片")
        self.import_image_button.setObjectName("BtnLocal")
        self.import_image_button.clicked.connect(self.on_import_local_image)
        self._apply_min_height(self.import_image_button)
        layout.addWidget(self.import_image_button)
        return card

    def _build_params_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("⚙️ 雕刻参数", "调整作品尺寸、速度与功率，适配不同课堂材料和演示需求。")

        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignLeft)
        form.setFormAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignTop)
        form.setHorizontalSpacing(12)
        form.setVerticalSpacing(10)

        self.width_spin = QtWidgets.QDoubleSpinBox()
        self.width_spin.setRange(1.0, 300.0)
        self.width_spin.setValue(50.0)
        self.width_spin.setSuffix(" mm")

        self.height_spin = QtWidgets.QDoubleSpinBox()
        self.height_spin.setRange(1.0, 300.0)
        self.height_spin.setValue(50.0)
        self.height_spin.setSuffix(" mm")

        self.feed_spin = QtWidgets.QSpinBox()
        self.feed_spin.setRange(1, 100000)
        self.feed_spin.setValue(6000)

        self.power_spin = QtWidgets.QSpinBox()
        self.power_spin.setRange(0, 1000)
        self.power_spin.setValue(200)

        self._apply_min_height(self.width_spin, self.height_spin, self.feed_spin, self.power_spin)

        form.addRow("宽度", self.width_spin)
        form.addRow("高度", self.height_spin)
        form.addRow("进给速度 F", self.feed_spin)
        form.addRow("激光功率 S", self.power_spin)
        layout.addLayout(form)
        return card

    def _build_action_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("🕹️ 操作控制", "生成 G-Code、预览边界、开始雕刻或在紧急情况下立即停止。")

        self.generate_gcode_button = QtWidgets.QPushButton("生成 G-Code")
        self.generate_gcode_button.clicked.connect(self.on_generate_gcode)

        self.preview_button = QtWidgets.QPushButton("🎯 红光边框预览")
        self.preview_button.clicked.connect(self.on_preview_bbox)

        self.start_button = QtWidgets.QPushButton("🚀 开始激光雕刻")
        self.start_button.setObjectName("BtnStart")
        self.start_button.clicked.connect(self.on_start_engraving)

        self.export_button = QtWidgets.QPushButton("导出 G-Code 文件")
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
        card, layout = self._create_card("🖼️ 作品预览", "上方展示原图与轮廓图，保持比例居中显示，便于课堂讲解与观察。")

        self.progress_bar = QtWidgets.QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setFormat("任务进度 %p%")
        layout.addWidget(self.progress_bar)

        preview_row = QtWidgets.QHBoxLayout()
        preview_row.setSpacing(12)

        left_preview = self._create_preview_panel("🖼️ AI 原图预览", "等待生成或导入图片")
        right_preview = self._create_preview_panel("✏️ 轮廓路径预览", "等待轮廓提取结果")

        self.original_image_label = left_preview[1]
        self.contour_image_label = right_preview[1]

        preview_row.addWidget(left_preview[0], 1)
        preview_row.addWidget(right_preview[0], 1)
        layout.addLayout(preview_row)
        return card

    def _create_preview_panel(self, title: str, placeholder: str) -> Tuple[QtWidgets.QFrame, ImageLabel]:
        panel = QtWidgets.QFrame()
        panel.setObjectName("PreviewPanel")
        panel_layout = QtWidgets.QVBoxLayout(panel)
        panel_layout.setContentsMargins(12, 12, 12, 12)
        panel_layout.setSpacing(8)

        title_label = QtWidgets.QLabel(title)
        title_label.setObjectName("PreviewTitle")
        panel_layout.addWidget(title_label)

        image_label = ImageLabel(placeholder)
        panel_layout.addWidget(image_label, 1)
        return panel, image_label

    def _build_log_card(self) -> QtWidgets.QFrame:
        card, layout = self._create_card("📝 系统消息板", "这里会显示串口收发、G-Code 预览和任务运行状态，便于教学演示。")

        self.log_edit = QtWidgets.QTextEdit()
        self.log_edit.setReadOnly(True)
        self.log_edit.setObjectName("LogBox")
        self.log_edit.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        layout.addWidget(self.log_edit, 1)
        return card

    def apply_stylesheet(self) -> None:
        self.setStyleSheet(BRIGHT_MAKER_QSS)

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
        self.import_image_button.setDisabled(generating)
        if generating:
            self.generate_image_button.setText("✨ 生成中...")
        else:
            self.generate_image_button.setText("✨ 魔法生成图像")

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
        self.append_log(f"{source_name}加载完成：{image_path}")
        self.append_log(f"轮廓提取完成，共 {len(contours)} 条路径，已完成降噪与居中排版。")

    def toggle_serial_connection(self) -> None:
        if self.connect_button.text() == "连接串口":
            port = self.port_combo.currentText().strip()
            if not port:
                QtWidgets.QMessageBox.warning(self, "提示", "请先选择可用串口")
                return
            try:
                self.serial_thread.connect_port(port, int(self.baud_combo.currentText()))
                self.connect_button.setText("断开串口")
                self.append_log(f"串口连接成功：{port}")
            except Exception as exc:
                QtWidgets.QMessageBox.critical(self, "串口错误", str(exc))
                self.append_log(f"[错误] {exc}")
        else:
            self.serial_thread.disconnect_port()
            self.connect_button.setText("连接串口")
            self.append_log("串口已断开。")

    def on_generate_image(self) -> None:
        prompt = self.prompt_edit.toPlainText().strip()
        if not prompt:
            QtWidgets.QMessageBox.warning(self, "提示", "请输入 AI 提示词")
            return

        self._set_ai_generating(True)
        self.ai_thread = AIGeneratorThread(prompt=prompt, parent=self)
        self.ai_thread.started_signal.connect(self.append_log)
        self.ai_thread.success_signal.connect(self.on_ai_image_generated)
        self.ai_thread.error_signal.connect(self.on_ai_image_error)
        self.ai_thread.finished.connect(lambda: self._set_ai_generating(False))
        self.ai_thread.finished.connect(self.ai_thread.deleteLater)
        self.ai_thread.start()

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
        self.append_log(f"AI 图像生成完成：{image_path}")
        self._load_image_into_pipeline(image_path, "AI 图像")

    def on_ai_image_error(self, message: str) -> None:
        QtWidgets.QMessageBox.critical(self, "生成失败", message)
        self.append_log(f"[错误] {message}")

    def on_generate_gcode(self) -> None:
        if not self.current_contours:
            QtWidgets.QMessageBox.warning(self, "提示", "请先生成图像并提取轮廓")
            return
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
            return

        preview = "\n".join(self.current_gcode_lines[:20])
        self.append_log("G-Code 生成完成，预览前 20 行：")
        self.append_log(preview)

    def on_preview_bbox(self) -> None:
        if not self.current_contours:
            QtWidgets.QMessageBox.warning(self, "提示", "请先生成图像并提取轮廓")
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
            self.append_log("已下发红光边框预览任务。")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "串口发送失败", str(exc))
            self.append_log(f"[错误] {exc}")

    def on_start_engraving(self) -> None:
        if not self.current_gcode_lines:
            self.on_generate_gcode()
            if not self.current_gcode_lines:
                return
        try:
            self.serial_thread.enqueue_gcode(self.current_gcode_lines)
            self.append_log("已下发雕刻任务。")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "串口发送失败", str(exc))
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
            self.append_log("如果现场不直接走串口，可把该文件导入 LaserGRBL 作为后备方案。")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "导出失败", str(exc))
            self.append_log(f"[错误] {exc}")

    def on_emergency_stop(self) -> None:
        self.serial_thread.emergency_stop()
        self.append_log("已触发急停。")

    def on_job_finished(self, success: bool, message: str) -> None:
        if success:
            self.append_log(f"[完成] {message}")
        else:
            self.append_log(f"[失败] {message}")
            QtWidgets.QMessageBox.warning(self, "任务状态", message)
