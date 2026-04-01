"""
主界面模块。

界面布局：
1. 左侧负责串口、AI、参数和操作控制；
2. 右侧负责原图/轮廓图预览与日志输出；
3. 保留导出 G-Code 与直接串口下发两条链路，兼容现阶段 LaserGRBL 习惯。
"""

from __future__ import annotations

from pathlib import Path
from typing import List, Optional

from .ai_image_generator import AIImageGenerationError, generate_image_from_text
from .gcode_generator import generate_gcode, generate_preview_gcode, save_gcode_file
from .image_processing import ImageProcessingError, NormalizedPath, process_image_to_contours
from .qt_compat import QT_API, QtCore, QtGui, QtWidgets
from .serial_worker import SerialWorkerThread


class ImageLabel(QtWidgets.QLabel):
    """用于显示图片的简单封装。"""

    def __init__(self, title: str, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self.setMinimumSize(360, 280)
        self.setAlignment(QtCore.Qt.AlignCenter)
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        self.setStyleSheet("background:#f7f7f7; color:#666;")
        self.setText(title)

    def set_image(self, image_path: str) -> None:
        pixmap = QtGui.QPixmap(image_path)
        if pixmap.isNull():
            self.setText(f"图片加载失败:\n{image_path}")
            return
        scaled = pixmap.scaled(self.size(), QtCore.Qt.KeepAspectRatio, QtCore.Qt.SmoothTransformation)
        self.setPixmap(scaled)

    def resizeEvent(self, event: QtGui.QResizeEvent) -> None:  # pragma: no cover - GUI 行为
        super().resizeEvent(event)
        if self.pixmap() is not None:
            self.setPixmap(self.pixmap().scaled(self.size(), QtCore.Qt.KeepAspectRatio, QtCore.Qt.SmoothTransformation))


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(f"AI 智能创作中枢 - {QT_API}")
        self.resize(1380, 860)

        self.current_image_path: Optional[str] = None
        self.current_contours_path: Optional[str] = None
        self.current_contours: List[NormalizedPath] = []
        self.current_gcode_lines: List[str] = []
        self.current_preview_lines: List[str] = []

        self.serial_thread = SerialWorkerThread(self)
        self.serial_thread.log_signal.connect(self.append_log)
        self.serial_thread.status_signal.connect(self.append_log)
        self.serial_thread.progress_signal.connect(self.on_progress_changed)
        self.serial_thread.ports_signal.connect(self.update_ports)
        self.serial_thread.finished_signal.connect(self.on_job_finished)
        self.serial_thread.start()

        self._build_ui()
        self.refresh_ports()

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # pragma: no cover - GUI 行为
        self.serial_thread.stop()
        super().closeEvent(event)

    def _build_ui(self) -> None:
        central = QtWidgets.QWidget()
        self.setCentralWidget(central)

        main_layout = QtWidgets.QHBoxLayout(central)

        left_panel = QtWidgets.QWidget()
        left_layout = QtWidgets.QVBoxLayout(left_panel)
        left_layout.setSpacing(12)
        left_layout.addWidget(self._build_serial_group())
        left_layout.addWidget(self._build_ai_group())
        left_layout.addWidget(self._build_params_group())
        left_layout.addWidget(self._build_action_group())
        left_layout.addStretch(1)

        right_panel = QtWidgets.QWidget()
        right_layout = QtWidgets.QVBoxLayout(right_panel)
        image_layout = QtWidgets.QHBoxLayout()
        self.original_image_label = ImageLabel("AI 原图预览")
        self.contour_image_label = ImageLabel("轮廓/边缘预览")
        image_layout.addWidget(self.original_image_label, 1)
        image_layout.addWidget(self.contour_image_label, 1)
        right_layout.addLayout(image_layout, 3)

        self.progress_bar = QtWidgets.QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        right_layout.addWidget(self.progress_bar)

        self.log_edit = QtWidgets.QPlainTextEdit()
        self.log_edit.setReadOnly(True)
        right_layout.addWidget(self.log_edit, 2)

        splitter = QtWidgets.QSplitter()
        splitter.addWidget(left_panel)
        splitter.addWidget(right_panel)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        main_layout.addWidget(splitter)

    def _build_serial_group(self) -> QtWidgets.QGroupBox:
        box = QtWidgets.QGroupBox("串口设置")
        form = QtWidgets.QFormLayout(box)

        self.port_combo = QtWidgets.QComboBox()
        self.refresh_ports_button = QtWidgets.QPushButton("刷新串口")
        self.refresh_ports_button.clicked.connect(self.refresh_ports)

        port_row = QtWidgets.QHBoxLayout()
        port_row.addWidget(self.port_combo, 1)
        port_row.addWidget(self.refresh_ports_button)
        form.addRow("串口", port_row)

        self.baud_combo = QtWidgets.QComboBox()
        self.baud_combo.addItems(["115200", "230400", "460800", "921600"])
        self.baud_combo.setCurrentText("115200")
        form.addRow("波特率", self.baud_combo)

        self.connect_button = QtWidgets.QPushButton("连接串口")
        self.connect_button.clicked.connect(self.toggle_serial_connection)
        form.addRow(self.connect_button)
        return box

    def _build_ai_group(self) -> QtWidgets.QGroupBox:
        box = QtWidgets.QGroupBox("AI 创作区")
        layout = QtWidgets.QVBoxLayout(box)
        self.prompt_edit = QtWidgets.QPlainTextEdit()
        self.prompt_edit.setPlaceholderText("请输入提示词，例如：画一只赛博朋克风格的猫")
        self.prompt_edit.setFixedHeight(110)
        layout.addWidget(self.prompt_edit)

        self.generate_image_button = QtWidgets.QPushButton("生成图像")
        self.generate_image_button.clicked.connect(self.on_generate_image)
        layout.addWidget(self.generate_image_button)
        return box

    def _build_params_group(self) -> QtWidgets.QGroupBox:
        box = QtWidgets.QGroupBox("雕刻参数")
        form = QtWidgets.QFormLayout(box)

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

        form.addRow("宽度", self.width_spin)
        form.addRow("高度", self.height_spin)
        form.addRow("进给速度 F", self.feed_spin)
        form.addRow("激光功率 S", self.power_spin)
        return box

    def _build_action_group(self) -> QtWidgets.QGroupBox:
        box = QtWidgets.QGroupBox("操作区")
        layout = QtWidgets.QVBoxLayout(box)

        self.generate_gcode_button = QtWidgets.QPushButton("生成 G-Code")
        self.generate_gcode_button.clicked.connect(self.on_generate_gcode)
        layout.addWidget(self.generate_gcode_button)

        self.preview_button = QtWidgets.QPushButton("红光边框预览")
        self.preview_button.clicked.connect(self.on_preview_bbox)
        layout.addWidget(self.preview_button)

        self.start_button = QtWidgets.QPushButton("开始雕刻")
        self.start_button.clicked.connect(self.on_start_engraving)
        layout.addWidget(self.start_button)

        self.export_button = QtWidgets.QPushButton("导出 G-Code 文件")
        self.export_button.clicked.connect(self.on_export_gcode)
        layout.addWidget(self.export_button)

        self.emergency_button = QtWidgets.QPushButton("急停")
        self.emergency_button.setStyleSheet("background:#d9534f; color:white; font-weight:bold;")
        self.emergency_button.clicked.connect(self.on_emergency_stop)
        layout.addWidget(self.emergency_button)

        return box

    def append_log(self, message: str) -> None:
        self.log_edit.appendPlainText(message)

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
        self.append_log("已刷新串口列表")

    def toggle_serial_connection(self) -> None:
        if self.connect_button.text() == "连接串口":
            port = self.port_combo.currentText().strip()
            if not port:
                QtWidgets.QMessageBox.warning(self, "提示", "请先选择可用串口")
                return
            try:
                self.serial_thread.connect_port(port, int(self.baud_combo.currentText()))
                self.connect_button.setText("断开串口")
                self.append_log(f"串口连接成功: {port}")
            except Exception as exc:
                QtWidgets.QMessageBox.critical(self, "串口错误", str(exc))
                self.append_log(f"[ERR] {exc}")
        else:
            self.serial_thread.disconnect_port()
            self.connect_button.setText("连接串口")
            self.append_log("串口已断开")

    def on_generate_image(self) -> None:
        prompt = self.prompt_edit.toPlainText().strip()
        if not prompt:
            QtWidgets.QMessageBox.warning(self, "提示", "请输入 AI 提示词")
            return

        try:
            image_path = generate_image_from_text(prompt)
            contours, contours_path = process_image_to_contours(image_path)
        except (AIImageGenerationError, ImageProcessingError) as exc:
            QtWidgets.QMessageBox.critical(self, "生成失败", str(exc))
            self.append_log(f"[ERR] {exc}")
            return
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "未知错误", str(exc))
            self.append_log(f"[ERR] 生成图像时出现未知异常: {exc}")
            return

        self.current_image_path = image_path
        self.current_contours_path = contours_path
        self.current_contours = contours
        self.original_image_label.set_image(image_path)
        self.contour_image_label.set_image(contours_path)
        self.append_log(f"AI 图像生成完成: {image_path}")
        self.append_log(f"轮廓提取完成，共 {len(contours)} 条路径")

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
            self.append_log(f"[ERR] {exc}")
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
            self.append_log(f"[ERR] {exc}")
            return

        try:
            self.serial_thread.enqueue_gcode(self.current_preview_lines)
            self.append_log("已下发红光边框预览任务")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "串口发送失败", str(exc))
            self.append_log(f"[ERR] {exc}")

    def on_start_engraving(self) -> None:
        if not self.current_gcode_lines:
            self.on_generate_gcode()
            if not self.current_gcode_lines:
                return
        try:
            self.serial_thread.enqueue_gcode(self.current_gcode_lines)
            self.append_log("已下发雕刻任务")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "串口发送失败", str(exc))
            self.append_log(f"[ERR] {exc}")

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
            self.append_log(f"G-Code 已导出: {saved}")
            self.append_log("如果现场不直接走串口，可把该文件导入 LaserGRBL 作为后备方案")
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "导出失败", str(exc))
            self.append_log(f"[ERR] {exc}")

    def on_emergency_stop(self) -> None:
        self.serial_thread.emergency_stop()
        self.append_log("已触发急停")

    def on_job_finished(self, success: bool, message: str) -> None:
        if success:
            self.append_log(f"[DONE] {message}")
        else:
            self.append_log(f"[FAIL] {message}")
            QtWidgets.QMessageBox.warning(self, "任务状态", message)

