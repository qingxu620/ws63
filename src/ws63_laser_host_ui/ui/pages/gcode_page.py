from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QImage, QPainter, QPen, QPixmap
from PySide6.QtWidgets import (
    QButtonGroup,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSlider,
    QSpinBox,
    QTabWidget,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from app.image_gcode import (
    Contour,
    raster_mask_to_gcode,
    prepare_laser_mask,
    simplify_vector_contours,
    trace_vector_contours_from_mask,
    vector_contours_to_gcode,
)


RX_JOB_MAX_BYTES = 65536
OUTLINE_SCAN_FEED_MM_MIN = 10000
OUTLINE_SCAN_POWER_S = 80
OUTLINE_SCAN_LOOPS = 2
SCANLINE_PROMPT_SUFFIX = (
    "用于激光打标扫描填充。请生成主体清晰、白色或浅色背景、明暗层次明确的图像，"
    "允许灰度和适度阴影，用高对比轮廓突出主体，背景保持简单。避免彩色杂乱背景、"
    "文字、复杂纹理、过密毛发和大面积噪点。"
)
VECTOR_PROMPT_SUFFIX = (
    "用于激光打标轮廓矢量提取。请生成主体边界清晰、白色或浅色背景、形体完整的"
    "插画、卡通或产品图，允许少量灰度层次，但要有明确外轮廓和主要内轮廓。"
    "避免照片级复杂纹理、碎线、低对比边缘、复杂背景和文字。"
)
_GCODE_WORD_RE = re.compile(
    r"([A-Z])\s*([-+]?(?:\d+(?:\.\d*)?|\.\d+))", re.IGNORECASE
)


@dataclass(frozen=True)
class _MarkBounds:
    min_x: float
    min_y: float
    max_x: float
    max_y: float

    @property
    def width(self) -> float:
        return self.max_x - self.min_x

    @property
    def height(self) -> float:
        return self.max_y - self.min_y


class GcodePage(QWidget):
    """AI/image workspace plus the existing plain-text G-code editor."""

    gcode_loaded = Signal(str, bytes)
    ai_generation_requested = Signal(str, str, bool, str)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._path = ""
        self._image_path = ""
        self._source_image = QImage()
        self._converted_image = QImage()
        self._preview_mode = "original"
        self._source_status = ""
        self._converted_status = ""
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setSpacing(12)
        layout.setContentsMargins(24, 20, 24, 20)

        subtitle = QLabel("利用 AI 生图、导入本地图片并转换为矢量 G-code 代码，或进行手工编辑与校验。")
        subtitle.setObjectName("pageSubtitle")
        subtitle.setWordWrap(True)
        layout.addWidget(subtitle)

        self.tabs = QTabWidget()
        self.tabs.setObjectName("gcodeTabs")
        self.tabs.setDocumentMode(True)
        self.tabs.addTab(self._build_image_tab(), "AI 生图与矢量化")
        self.tabs.addTab(self._build_editor_tab(), "G-code 编辑器")
        layout.addWidget(self.tabs, 1)

    def _build_image_tab(self) -> QWidget:
        tab = QWidget()
        root = QHBoxLayout(tab)
        root.setContentsMargins(0, 12, 0, 0)
        root.setSpacing(22)

        controls_scroll = QScrollArea()
        controls_scroll.setObjectName("aiControlsScroll")
        controls_scroll.setWidgetResizable(True)
        controls_scroll.setFrameShape(QFrame.Shape.NoFrame)
        controls_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        controls_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        controls_scroll.setFixedWidth(340)
        controls_scroll.setStyleSheet(
            "QScrollArea#aiControlsScroll { background:transparent; border:none; }"
            "QScrollArea#aiControlsScroll > QWidget > QWidget { background:transparent; }"
        )

        controls_widget = QWidget()
        controls = QVBoxLayout(controls_widget)
        controls.setContentsMargins(0, 0, 10, 0)
        controls.setSpacing(18)

        prompt_group = QGroupBox("提示词输入")
        prompt_group.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        prompt_layout = QVBoxLayout(prompt_group)
        prompt_layout.setContentsMargins(16, 22, 16, 14)
        prompt_layout.setSpacing(10)
        self.prompt_edit = QTextEdit()
        self.prompt_edit.setAcceptRichText(False)
        self.prompt_edit.setPlaceholderText("例如：极简线条狼头，逼真黑色几何线条，白底简笔画...")
        self.prompt_edit.setPlainText("极简线条狼头，黑白简笔矢量风格")
        self.prompt_edit.setFixedHeight(64)
        prompt_layout.addWidget(self.prompt_edit)

        preset_row = QHBoxLayout()
        preset_row.setSpacing(8)
        self.prompt_preset_combo = QComboBox()
        self.prompt_preset_combo.setObjectName("promptPresetMode")
        self.prompt_preset_combo.addItem("跟随提取模式", "auto")
        self.prompt_preset_combo.addItem("扫描填充优化", "scanline")
        self.prompt_preset_combo.addItem("轮廓矢量优化", "vector")
        self.prompt_preset_combo.addItem("仅使用原提示词", "raw")
        preset_row.addWidget(QLabel("模板"))
        preset_row.addWidget(self.prompt_preset_combo, 1)
        prompt_layout.addLayout(preset_row)

        gen_options = QHBoxLayout()
        gen_options.setSpacing(8)
        self.image_size_combo = QComboBox()
        self.image_size_combo.setObjectName("doubaoImageSize")
        self.image_size_combo.addItems(["2k", "3k", "4k"])
        self.image_size_combo.setCurrentText("2k")
        gen_options.addWidget(QLabel("尺寸"))
        gen_options.addWidget(self.image_size_combo, 1)
        self.watermark_check = QCheckBox("加水印")
        self.watermark_check.setChecked(True)
        gen_options.addWidget(self.watermark_check)
        prompt_layout.addLayout(gen_options)

        output_row = QHBoxLayout()
        output_row.setSpacing(8)
        self.output_dir_edit = QLineEdit()
        self.output_dir_edit.setObjectName("doubaoOutputDir")
        default_output_dir = Path(__file__).resolve().parents[2] / "generated_images"
        self.output_dir_edit.setText(str(default_output_dir))
        self.output_dir_edit.setPlaceholderText("生成图片保存目录")
        output_row.addWidget(self.output_dir_edit, 1)
        self.btn_output_dir = QPushButton("目录")
        self.btn_output_dir.clicked.connect(self._choose_output_dir)
        output_row.addWidget(self.btn_output_dir)
        prompt_layout.addLayout(output_row)

        prompt_buttons = QHBoxLayout()
        self.btn_generate = QPushButton("生成图像")
        self.btn_generate.setObjectName("btnPrimary")
        self.btn_generate.clicked.connect(self._request_ai_image)
        prompt_buttons.addWidget(self.btn_generate, 1)
        self.btn_import_image = QPushButton("导入图片")
        self.btn_import_image.clicked.connect(self._import_image)
        prompt_buttons.addWidget(self.btn_import_image, 1)
        prompt_layout.addLayout(prompt_buttons)
        controls.addWidget(prompt_group)

        params_group = QGroupBox("矢量化参数设置")
        params_group.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        params_layout = QVBoxLayout(params_group)
        params_layout.setContentsMargins(16, 22, 16, 14)
        params_layout.setSpacing(10)
        mode_caption = QLabel("提取模式")
        mode_caption.setStyleSheet("font-size:12px; font-weight:600; color:#475569;")
        params_layout.addWidget(mode_caption)
        self.extract_mode = QComboBox()
        self.extract_mode.setObjectName("extractMode")
        self.extract_mode.addItem("扫描填充（逐行雕刻）", "scanline")
        self.extract_mode.addItem("轮廓矢量（沿边界雕刻）", "vector")
        params_layout.addWidget(self.extract_mode)
        self.threshold_slider, self.threshold_spin = self._parameter_row(
            params_layout, "提取阈值 (0-255)", 0, 255, 128
        )
        self.size_slider, self.size_spin = self._parameter_row(
            params_layout, "正方形边长 (mm，0×0 至 99×99)", 0, 99, 99
        )
        self.speed_slider, self.speed_spin = self._parameter_row(
            params_layout, "工件雕刻速度 (mm/min)", 100, 12_000, 3000
        )
        self.power_slider, self.power_spin = self._parameter_row(
            params_layout, "最大功率 S (0-1000)", 0, 1000, 1000
        )
        vector_row = QHBoxLayout()
        vector_row.setSpacing(8)
        self.vector_simplify_label = QLabel("路径简化 (px)")
        self.vector_simplify_label.setStyleSheet("font-size:12px; font-weight:600; color:#475569;")
        vector_row.addWidget(self.vector_simplify_label, 1)
        self.vector_simplify = QDoubleSpinBox()
        self.vector_simplify.setObjectName("vectorSimplify")
        self.vector_simplify.setRange(0.0, 5.0)
        self.vector_simplify.setSingleStep(0.1)
        self.vector_simplify.setDecimals(1)
        self.vector_simplify.setValue(1.0)
        self.vector_simplify.setFixedWidth(78)
        vector_row.addWidget(self.vector_simplify)
        params_layout.addLayout(vector_row)

        noise_row = QHBoxLayout()
        noise_row.setSpacing(8)
        self.vector_noise_label = QLabel("去噪面积 (px²)")
        self.vector_noise_label.setStyleSheet("font-size:12px; font-weight:600; color:#475569;")
        noise_row.addWidget(self.vector_noise_label, 1)
        self.vector_min_area = QSpinBox()
        self.vector_min_area.setObjectName("vectorMinArea")
        self.vector_min_area.setRange(1, 1000)
        self.vector_min_area.setValue(4)
        self.vector_min_area.setFixedWidth(78)
        noise_row.addWidget(self.vector_min_area)
        params_layout.addLayout(noise_row)

        self.extract_mode.currentIndexChanged.connect(self._update_vector_controls)
        self._update_vector_controls()
        self.btn_convert = QPushButton("转换为 G-code")
        self.btn_convert.setObjectName("btnAccent")
        self.btn_convert.clicked.connect(self._convert_image)
        params_layout.addWidget(self.btn_convert)
        controls.addWidget(params_group)
        controls.addStretch(1)
        controls_widget.setMinimumHeight(
            prompt_group.sizeHint().height()
            + params_group.sizeHint().height()
            + controls.spacing()
        )
        controls_scroll.setWidget(controls_widget)
        root.addWidget(controls_scroll, 0)

        preview = QFrame()
        preview.setObjectName("imagePreviewCard")
        preview_layout = QVBoxLayout(preview)
        preview_layout.setContentsMargins(28, 28, 28, 28)

        preview_toolbar = QHBoxLayout()
        preview_title = QLabel("图像预览")
        preview_title.setStyleSheet("color:#475569; font-size:12px; font-weight:700; border:none;")
        preview_toolbar.addWidget(preview_title)
        preview_toolbar.addStretch(1)
        self.preview_buttons = QButtonGroup(self)
        self.preview_buttons.setExclusive(True)
        self.btn_preview_original = QPushButton("原图")
        self.btn_preview_original.setCheckable(True)
        self.btn_preview_original.setEnabled(False)
        self.btn_preview_original.clicked.connect(
            lambda: self._set_preview_mode("original")
        )
        self.preview_buttons.addButton(self.btn_preview_original)
        preview_toolbar.addWidget(self.btn_preview_original)
        self.btn_preview_converted = QPushButton("转换效果")
        self.btn_preview_converted.setCheckable(True)
        self.btn_preview_converted.setEnabled(False)
        self.btn_preview_converted.clicked.connect(
            lambda: self._set_preview_mode("converted")
        )
        self.preview_buttons.addButton(self.btn_preview_converted)
        preview_toolbar.addWidget(self.btn_preview_converted)
        preview_layout.addLayout(preview_toolbar)
        preview.setStyleSheet(
            "QFrame#imagePreviewCard { background:#ffffff; border:1px dashed #cbd5e1; "
            "border-radius:12px; }"
            "QFrame#imagePreviewCard QPushButton { padding:6px 12px; }"
            "QFrame#imagePreviewCard QPushButton:checked { background:#0284c7; color:white; "
            "border-color:#0284c7; }"
        )
        self.preview_label = QLabel("▧\n\n未加载图像预览\n\n请在左侧请求生成图像，或者从本地选择图片导入")
        self.preview_label.setAlignment(Qt.AlignCenter)
        self.preview_label.setWordWrap(True)
        self.preview_label.setStyleSheet("color:#94a3b8; font-size:14px; border:none;")
        preview_layout.addWidget(self.preview_label, 1)
        self.image_status = QLabel("")
        self.image_status.setAlignment(Qt.AlignCenter)
        self.image_status.setStyleSheet("color:#0284c7; font-size:12px; border:none;")
        preview_layout.addWidget(self.image_status)
        root.addWidget(preview, 1)
        return tab

    @staticmethod
    def _parameter_row(
        parent: QVBoxLayout, label: str, minimum: int, maximum: int, value: int
    ) -> tuple[QSlider, QSpinBox]:
        caption = QLabel(label)
        caption.setStyleSheet("font-size:12px; font-weight:600; color:#475569;")
        parent.addWidget(caption)
        row = QHBoxLayout()
        slider = QSlider(Qt.Horizontal)
        slider.setRange(minimum, maximum)
        slider.setValue(value)
        spin = QSpinBox()
        spin.setRange(minimum, maximum)
        spin.setValue(value)
        spin.setFixedWidth(78)
        slider.valueChanged.connect(spin.setValue)
        spin.valueChanged.connect(slider.setValue)
        row.addWidget(slider, 1)
        row.addWidget(spin)
        parent.addLayout(row)
        return slider, spin

    def _build_editor_tab(self) -> QWidget:
        tab = QWidget()
        layout = QVBoxLayout(tab)
        layout.setContentsMargins(0, 12, 0, 0)
        layout.setSpacing(18)

        toolbar_card = QFrame()
        toolbar_card.setObjectName("toolbarCard")
        toolbar_card.setStyleSheet(
            "QFrame#toolbarCard { background:#ffffff; border:1px solid #e2e8f0; border-radius:9px; }"
        )
        toolbar = QHBoxLayout(toolbar_card)
        toolbar.setContentsMargins(16, 12, 16, 12)
        toolbar.setSpacing(12)
        self.btn_open = QPushButton("打开文件")
        self.btn_open.setObjectName("btnPrimary")
        self.btn_open.clicked.connect(self._open_file)
        toolbar.addWidget(self.btn_open)
        self.btn_clear = QPushButton("清空")
        self.btn_clear.clicked.connect(self._clear)
        toolbar.addWidget(self.btn_clear)
        self.lbl_file = QLabel("未加载文件")
        self.lbl_file.setObjectName("filePath")
        self.lbl_file.setStyleSheet("color:#475569; border:none;")
        toolbar.addWidget(self.lbl_file, 1)
        self.lbl_size = QLabel("")
        self.lbl_size.setObjectName("fileSize")
        self.lbl_size.setStyleSheet("color:#0284c7; font-weight:600; border:none;")
        toolbar.addWidget(self.lbl_size)
        layout.addWidget(toolbar_card)

        editor_card = QFrame()
        editor_card.setObjectName("editorCard")
        editor_card.setStyleSheet(
            "QFrame#editorCard { background:#ffffff; border:1px solid #e2e8f0; border-radius:10px; }"
        )
        editor_layout = QVBoxLayout(editor_card)
        editor_layout.setContentsMargins(0, 0, 0, 0)
        editor_layout.setSpacing(0)
        editor_header = QLabel("代码编辑器 (纯文本形式)")
        editor_header.setObjectName("terminalHeader")
        editor_header.setStyleSheet(
            "background:#f8fafc; color:#475569; border:none; border-bottom:1px solid #e2e8f0; "
            "padding:10px 14px; font-size:12px; font-weight:600;"
        )
        editor_layout.addWidget(editor_header)
        self.editor = QTextEdit()
        self.editor.setPlaceholderText("在此粘贴 G-code 或点击打开文件...")
        self.editor.setAcceptRichText(False)
        self.editor.setFontFamily("Consolas")
        self.editor.setStyleSheet("border:none; border-radius:0; background:#ffffff;")
        self.editor.textChanged.connect(self._update_size)
        editor_layout.addWidget(self.editor, 1)
        layout.addWidget(editor_card, 1)
        return tab

    def _update_vector_controls(self) -> None:
        enabled = self.extract_mode.currentData() == "vector"
        for widget in (
            self.vector_simplify_label,
            self.vector_simplify,
            self.vector_noise_label,
            self.vector_min_area,
        ):
            widget.setEnabled(enabled)

    def get_gcode_bytes(self) -> bytes:
        text = self.editor.toPlainText().replace("\r\n", "\n").replace("\r", "\n")
        return text.encode("utf-8")

    def get_path(self) -> str:
        return self._path

    def set_content(self, path: str, data: bytes) -> None:
        self._path = path
        self.editor.blockSignals(True)
        self.editor.setPlainText(data.decode("utf-8", errors="replace"))
        self.editor.blockSignals(False)
        self.lbl_file.setText(path or "未加载文件")
        self._update_size()
        self.gcode_loaded.emit(path, data)

    def set_image(self, path: str, image: QImage) -> None:
        if image.isNull():
            raise ValueError("无法解码图像")
        self._image_path = path
        self._source_image = image
        self._converted_image = QImage()
        self._source_status = (
            f"原图 · {Path(path).name if path else '生成图像'} · "
            f"{image.width()}×{image.height()} px"
        )
        self._converted_status = ""
        self.btn_preview_original.setEnabled(True)
        self.btn_preview_converted.setEnabled(False)
        self._set_preview_mode("original")

    def show_ai_unavailable(self) -> None:
        self.image_status.setText("未配置 AI 图像服务；可使用“导入图片”完成本地转换")

    def update_ui_generating_state(self, is_generating: bool) -> None:
        self.btn_generate.setEnabled(not is_generating)
        self.btn_import_image.setEnabled(not is_generating)
        self.btn_output_dir.setEnabled(not is_generating)
        self.prompt_edit.setEnabled(not is_generating)
        self.prompt_preset_combo.setEnabled(not is_generating)
        self.image_size_combo.setEnabled(not is_generating)
        self.watermark_check.setEnabled(not is_generating)
        self.output_dir_edit.setEnabled(not is_generating)
        if is_generating:
            self.image_status.setText("生成中...")

    def _request_ai_image(self) -> None:
        prompt = self.prompt_edit.toPlainText().strip()
        if not prompt:
            self.image_status.setText("请先输入图像提示词")
            return
        output_dir = self.output_dir_edit.text().strip()
        if not output_dir:
            self.image_status.setText("请选择生成图片输出目录")
            return
        self.update_ui_generating_state(True)
        self.ai_generation_requested.emit(
            self._compose_generation_prompt(prompt),
            self.image_size_combo.currentText(),
            self.watermark_check.isChecked(),
            output_dir,
        )

    def _compose_generation_prompt(self, prompt: str) -> str:
        mode = self.prompt_preset_combo.currentData()
        if mode == "auto":
            mode = self.extract_mode.currentData()
        if mode == "scanline":
            suffix = SCANLINE_PROMPT_SUFFIX
        elif mode == "vector":
            suffix = VECTOR_PROMPT_SUFFIX
        else:
            return prompt
        return f"主题：{prompt}\n生成要求：{suffix}"

    def _choose_output_dir(self) -> None:
        path = QFileDialog.getExistingDirectory(
            self,
            "选择生成图片输出目录",
            self.output_dir_edit.text().strip() or str(Path.home()),
        )
        if path:
            self.output_dir_edit.setText(path)

    def _import_image(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self,
            "导入图像",
            "",
            "图像文件 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*.*)",
        )
        if path:
            image = QImage(path)
            if image.isNull():
                self.image_status.setText("图像读取失败")
                return
            self.set_image(path, image)

    def _convert_image(self) -> None:
        if self._source_image.isNull():
            self.image_status.setText("请先生成或导入图像")
            return
        image = self._source_image.scaled(
            320,
            320,
            Qt.KeepAspectRatio,
            Qt.SmoothTransformation,
        )
        rows = [
            [image.pixelColor(x, y).lightness() for x in range(image.width())]
            for y in range(image.height())
        ]
        mode = self.extract_mode.currentData()
        size_mm = float(self.size_spin.value())
        path_count = 0
        point_count = 0
        if size_mm == 0:
            text = "M4 S0\nM5\n"
            self._converted_image = QImage(
                image.width(), image.height(), QImage.Format.Format_RGB32
            )
            self._converted_image.fill(QColor("white"))
            mode_label = "轮廓矢量" if mode == "vector" else "扫描填充"
            suffix = "vector" if mode == "vector" else "scanline"
            self._converted_status = (
                f"{mode_label} · 0×0 mm · 安全空任务，不生成移动或开光路径 · "
                f"{len(text.encode('utf-8'))} bytes"
            )
            source = Path(self._image_path).stem if self._image_path else "generated_image"
            self.set_content(f"{source}.{suffix}.generated.gcode", text.encode("utf-8"))
            self.btn_preview_converted.setEnabled(True)
            self._set_preview_mode("converted")
            return
        if mode == "vector":
            mask = prepare_laser_mask(
                rows,
                self.threshold_spin.value(),
                adaptive=True,
                edge_enhance=True,
                min_area_px=float(self.vector_min_area.value()),
            )
            contours = trace_vector_contours_from_mask(
                mask, min_area_px=float(self.vector_min_area.value())
            )
            contours = simplify_vector_contours(
                contours, tolerance_px=self.vector_simplify.value()
            )
            text = vector_contours_to_gcode(
                contours,
                image.width(),
                image.height(),
                size_mm,
                self.speed_spin.value(),
                height_mm=size_mm,
                power_s=self.power_spin.value(),
            )
            self._converted_image = self._build_vector_preview(rows, contours)
            mode_label = "轮廓矢量"
            path_count = len(contours)
            point_count = sum(max(0, len(contour) - 1) for contour in contours)
            suffix = "vector"
        else:
            mask = prepare_laser_mask(
                rows,
                self.threshold_spin.value(),
                adaptive=True,
                edge_enhance=False,
                min_area_px=4,
            )
            text = raster_mask_to_gcode(
                mask,
                size_mm,
                self.speed_spin.value(),
                height_mm=size_mm,
                power_s=self.power_spin.value(),
            )
            self._converted_image = self._build_mask_preview(mask)
            mode_label = "扫描填充"
            suffix = "scanline"
        step_mm = min(
            self.size_spin.value() / image.width(),
            self.size_spin.value() / image.height(),
        )
        actual_width = image.width() * step_mm
        actual_height = image.height() * step_mm
        output_bytes = len(text.encode("utf-8"))
        algorithm = "边缘增强" if mode == "vector" else "自适应阈值"
        vector_stats = f" · 路径 {path_count} · 节点 {point_count}" if mode == "vector" else ""
        size_warning = " · ⚠ 超过64KiB上传上限" if output_bytes > RX_JOB_MAX_BYTES else ""
        self._converted_status = (
            f"{mode_label} · {algorithm} · X 0–{self.size_spin.value()} mm · "
            f"Y 0–{self.size_spin.value()} mm · "
            f"图形 {actual_width:.1f}×{actual_height:.1f} mm · "
            f"阈值 {self.threshold_spin.value()} · S{self.power_spin.value()}"
            f"{vector_stats} · {output_bytes} bytes{size_warning}"
        )
        source = Path(self._image_path).stem if self._image_path else "generated_image"
        self.set_content(f"{source}.{suffix}.generated.gcode", text.encode("utf-8"))
        self.btn_preview_converted.setEnabled(True)
        self._set_preview_mode("converted")

    def build_outline_scan_payload(self) -> tuple[str, bytes] | None:
        source = self.editor.toPlainText()
        bounds = self._mark_bounds_from_gcode(source)
        if bounds is None:
            self.image_status.setText("当前 G-code 没有可扫描的开光外框")
            return None
        if bounds.width < 0.05 or bounds.height < 0.05:
            self.image_status.setText("开光外框太小，无法扫描")
            return None
        text = self._build_outline_scan_gcode(bounds)
        source_name = Path(self._path).stem if self._path else "outline"
        data = text.encode("utf-8")
        self.image_status.setText(
            f"外框扫描 · X {bounds.min_x:.2f}-{bounds.max_x:.2f} mm · "
            f"Y {bounds.min_y:.2f}-{bounds.max_y:.2f} mm · "
            f"F{OUTLINE_SCAN_FEED_MM_MIN} · S{OUTLINE_SCAN_POWER_S} · {OUTLINE_SCAN_LOOPS}圈"
        )
        return f"{source_name}.outline_scan.gcode", data

    @staticmethod
    def _mark_bounds_from_gcode(text: str) -> _MarkBounds | None:
        x = 0.0
        y = 0.0
        laser_power = 0.0
        laser_modal_on = False
        absolute = True
        bounds: list[float] | None = None

        def include_segment(x0: float, y0: float, x1: float, y1: float) -> None:
            nonlocal bounds
            values = (x0, y0, x1, y1)
            if any(value < -0.001 or value > 99.001 for value in values):
                return
            if bounds is None:
                bounds = [min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)]
                return
            bounds[0] = min(bounds[0], x0, x1)
            bounds[1] = min(bounds[1], y0, y1)
            bounds[2] = max(bounds[2], x0, x1)
            bounds[3] = max(bounds[3], y0, y1)

        for raw_line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
            line = raw_line.split(";", 1)[0].strip().upper()
            if not line or line.startswith("("):
                continue
            words = {match.group(1).upper(): float(match.group(2)) for match in _GCODE_WORD_RE.finditer(line)}
            if not words:
                continue
            command = None
            if "G" in words:
                command = f"G{int(words['G'])}"
                if command == "G90":
                    absolute = True
                elif command == "G91":
                    absolute = False
            if "M" in words:
                mcode = int(words["M"])
                if mcode in (3, 4):
                    laser_modal_on = True
                    laser_power = float(words.get("S", laser_power))
                elif mcode == 5:
                    laser_modal_on = False
                    laser_power = 0.0
            if "S" in words:
                laser_power = float(words["S"])
            new_x = x
            new_y = y
            has_xy = "X" in words or "Y" in words
            if "X" in words:
                new_x = words["X"] if absolute else x + words["X"]
            if "Y" in words:
                new_y = words["Y"] if absolute else y + words["Y"]
            if command == "G1" and has_xy and laser_modal_on and laser_power > 0:
                include_segment(x, y, new_x, new_y)
            if command in ("G0", "G1") and has_xy:
                x = new_x
                y = new_y

        if bounds is None:
            return None
        return _MarkBounds(bounds[0], bounds[1], bounds[2], bounds[3])

    @staticmethod
    def _build_outline_scan_gcode(bounds: _MarkBounds) -> str:
        lines = [
            "M4 S0",
            f"F{OUTLINE_SCAN_FEED_MM_MIN}",
            f"G0 X{bounds.min_x:.3f} Y{bounds.min_y:.3f}",
            f"S{OUTLINE_SCAN_POWER_S}",
        ]
        for _ in range(OUTLINE_SCAN_LOOPS):
            lines.extend(
                [
                    f"G1 X{bounds.max_x:.3f} Y{bounds.min_y:.3f}",
                    f"G1 X{bounds.max_x:.3f} Y{bounds.max_y:.3f}",
                    f"G1 X{bounds.min_x:.3f} Y{bounds.max_y:.3f}",
                    f"G1 X{bounds.min_x:.3f} Y{bounds.min_y:.3f}",
                ]
            )
        lines.extend(("S0", "M5"))
        return "\n".join(lines) + "\n"

    @staticmethod
    def _build_effect_preview(rows: list[list[int]], threshold: int) -> QImage:
        height = len(rows)
        width = len(rows[0])
        effect = QImage(width, height, QImage.Format.Format_RGB32)
        effect.fill(QColor("white"))
        dark = QColor("#0f172a")
        for y, row in enumerate(rows):
            for x, value in enumerate(row):
                if value <= threshold:
                    effect.setPixelColor(x, y, dark)
        return effect

    @staticmethod
    def _build_mask_preview(mask: list[list[bool]]) -> QImage:
        height = len(mask)
        width = len(mask[0])
        effect = QImage(width, height, QImage.Format.Format_RGB32)
        effect.fill(QColor("white"))
        dark = QColor("#0f172a")
        for y, row in enumerate(mask):
            for x, is_dark in enumerate(row):
                if is_dark:
                    effect.setPixelColor(x, y, dark)
        return effect

    @staticmethod
    def _build_vector_preview(rows: list[list[int]], contours: list[Contour]) -> QImage:
        height = len(rows)
        width = len(rows[0])
        effect = QImage(width + 1, height + 1, QImage.Format.Format_RGB32)
        effect.fill(QColor("white"))
        painter = QPainter(effect)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setPen(QPen(QColor("#ef4444"), 1))
        for contour in contours:
            for first, second in zip(contour, contour[1:]):
                painter.drawLine(first[0], first[1], second[0], second[1])
        painter.end()
        return effect

    def _set_preview_mode(self, mode: str) -> None:
        if mode == "converted" and self._converted_image.isNull():
            return
        if mode == "original" and self._source_image.isNull():
            return
        self._preview_mode = mode
        self.btn_preview_original.setChecked(mode == "original")
        self.btn_preview_converted.setChecked(mode == "converted")
        self.image_status.setText(
            self._converted_status if mode == "converted" else self._source_status
        )
        self._render_preview()

    def _render_preview(self) -> None:
        image = (
            self._converted_image
            if self._preview_mode == "converted"
            else self._source_image
        )
        if image.isNull():
            return
        target = self.preview_label.size()
        pixmap = QPixmap.fromImage(image).scaled(
            max(80, target.width() - 20),
            max(80, target.height() - 20),
            Qt.KeepAspectRatio,
            Qt.SmoothTransformation,
        )
        self.preview_label.setText("")
        self.preview_label.setPixmap(pixmap)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        if not self._source_image.isNull():
            self._render_preview()

    def _open_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "打开 G-code 文件", "", "G-code (*.gcode *.nc *.txt);;所有文件 (*.*)"
        )
        if path:
            self.set_content(path, Path(path).read_bytes())

    def _clear(self) -> None:
        self._path = ""
        self.editor.clear()
        self.lbl_file.setText("未加载文件")
        self.lbl_size.setText("")
        self.gcode_loaded.emit("", b"")

    def _update_size(self) -> None:
        size = len(self.editor.toPlainText().encode("utf-8"))
        self.lbl_size.setText(f"{size} 字节" if size else "")
