from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re

from PySide6.QtCore import Qt, Signal, QRegularExpression
from PySide6.QtGui import QColor, QImage, QPainter, QPen, QPixmap, QSyntaxHighlighter, QTextCharFormat, QFont
from PySide6.QtWidgets import (
    QButtonGroup,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
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
    LaserGrblVectorOptions,
    apply_lasergrbl_tone,
    centerline_mask,
    dither_rows_to_mask,
    lasergrbl_style_vectorize,
    raster_mask_to_gcode,
    prepare_laser_mask,
    simplify_vector_contours,
    trace_vector_contours_from_mask,
    vector_contours_to_gcode,
)


RX_JOB_MAX_BYTES = 100 * 1024
OUTLINE_SCAN_FEED_MM_MIN = 10000
OUTLINE_SCAN_POWER_S = 80
OUTLINE_SCAN_LOOPS = 2
SCANLINE_PROMPT_SUFFIX = (
    "用于激光打标扫描填充。请生成主体清晰、色彩鲜艳饱满且饱和度高、白色或浅色干净背景、"
    "明暗层次及色彩层次明确的图像，允许灰度过渡，用高对比轮廓和鲜亮色彩突出主体，背景保持纯净简单。"
    "避免彩色杂乱无序背景、文字、复杂纹理、过密毛发和大面积噪点。"
)
VECTOR_PROMPT_SUFFIX = (
    "用于激光打标轮廓矢量提取。请生成主体边界清晰、色彩鲜丽活泼、对比度高、白色或单色干净背景、"
    "形体完整的插画、卡通或矢量风格设计，具有明确的彩色色块、主要内轮廓以及清晰的内外轮廓。"
    "避免照片级复杂纹理、碎线、低对比度边缘、复杂背景和文字。"
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
        self.setAcceptDrops(True)
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
        self.prompt_edit.setFixedHeight(78)
        prompt_layout.addWidget(self.prompt_edit)

        preset_row = QHBoxLayout()
        preset_row.setContentsMargins(0, 4, 0, 0)
        preset_row.setSpacing(8)
        self.prompt_preset_combo = QComboBox()
        self.prompt_preset_combo.setObjectName("promptPresetMode")
        self.prompt_preset_combo.addItem("跟随提取模式", "auto")
        self.prompt_preset_combo.addItem("扫描填充优化", "scanline")
        self.prompt_preset_combo.addItem("轮廓矢量优化", "vector")
        self.prompt_preset_combo.addItem("仅使用原提示词", "raw")
        preset_label = QLabel("模板")
        preset_label.setFixedWidth(42)
        preset_row.addWidget(preset_label)
        preset_row.addWidget(self.prompt_preset_combo, 1)
        prompt_layout.addLayout(preset_row)

        gen_options = QHBoxLayout()
        gen_options.setSpacing(8)
        self.image_size_combo = QComboBox()
        self.image_size_combo.setObjectName("doubaoImageSize")
        self.image_size_combo.addItems(["2k", "3k", "4k"])
        self.image_size_combo.setCurrentText("2k")
        size_label = QLabel("尺寸")
        size_label.setFixedWidth(42)
        gen_options.addWidget(size_label)
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

        params_group = QGroupBox("图像与目标参数")
        params_group.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        params_layout = QVBoxLayout(params_group)
        params_layout.setContentsMargins(16, 22, 16, 14)
        params_layout.setSpacing(10)

        mode_caption = QLabel("转换工具")
        mode_caption.setStyleSheet("font-size:12px; font-weight:600; color:#475569;")
        params_layout.addWidget(mode_caption)
        self.extract_mode = QComboBox()
        self.extract_mode.setObjectName("extractMode")
        self.extract_mode.addItem("线到线跟踪", "scanline")
        self.extract_mode.addItem("1bit B/W 抖动", "dither")
        self.extract_mode.addItem("矢量化（Potrace风格）", "lasergrbl_vector")
        self.extract_mode.addItem("中心线", "centerline")
        self.extract_mode.addItem("直通", "passthrough")
        self.extract_mode.addItem("轮廓矢量（边缘增强）", "vector")
        params_layout.addWidget(self.extract_mode)
        self.threshold_slider, self.threshold_spin = self._parameter_row(
            params_layout, "提取阈值 (0-255)", 0, 255, 128
        )

        self._build_hidden_image_parameter_state()
        self._build_hidden_target_parameter_state()

        dialog_buttons = QHBoxLayout()
        dialog_buttons.setSpacing(8)
        self.btn_image_params = QPushButton("图像参数...")
        self.btn_image_params.clicked.connect(self._open_image_parameter_dialog)
        dialog_buttons.addWidget(self.btn_image_params, 1)
        self.btn_target_params = QPushButton("目标参数...")
        self.btn_target_params.clicked.connect(self._open_target_parameter_dialog)
        dialog_buttons.addWidget(self.btn_target_params, 1)
        params_layout.addLayout(dialog_buttons)
        controls.addWidget(params_group)

        self.size_slider = None
        self.size_spin.valueChanged.connect(self._sync_locked_height)
        self.auto_size_check.toggled.connect(self._apply_auto_size_if_needed)
        self.dpi_spin.valueChanged.connect(self._apply_auto_size_if_needed)

        self.extract_mode.currentIndexChanged.connect(self._update_vector_controls)
        self._update_vector_controls()
        self.btn_convert = QPushButton("转换为 G-code")
        self.btn_convert.setObjectName("btnAccent")
        self.btn_convert.clicked.connect(self._convert_image)
        controls.addWidget(self.btn_convert)
        controls.addStretch(1)
        controls.activate()
        controls_widget.setMinimumHeight(controls.sizeHint().height())
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
        self.image_status.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        self.image_status.setWordWrap(True)
        self.image_status.setMinimumHeight(40)
        self.image_status.setMaximumHeight(56)
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

    def _build_hidden_image_parameter_state(self) -> None:
        self.resize_mode_combo = QComboBox(self)
        self.resize_mode_combo.setObjectName("resizeMode")
        self.resize_mode_combo.addItem("Sharp (NearestNeighbor)", "nearest")
        self.resize_mode_combo.addItem("Smooth (Bilinear)", "smooth")
        self.resize_mode_combo.hide()

        self.lasergrbl_brightness_spin = QSpinBox(self)
        self.lasergrbl_brightness_spin.setRange(-100, 100)
        self.lasergrbl_brightness_spin.setValue(0)
        self.lasergrbl_brightness_spin.hide()
        self.lasergrbl_contrast_spin = QSpinBox(self)
        self.lasergrbl_contrast_spin.setRange(-100, 100)
        self.lasergrbl_contrast_spin.setValue(0)
        self.lasergrbl_contrast_spin.hide()
        self.lasergrbl_white_clip_spin = QSpinBox(self)
        self.lasergrbl_white_clip_spin.setRange(0, 255)
        self.lasergrbl_white_clip_spin.setValue(245)
        self.lasergrbl_white_clip_spin.hide()
        self.lasergrbl_black_clip_spin = QSpinBox(self)
        self.lasergrbl_black_clip_spin.setRange(0, 255)
        self.lasergrbl_black_clip_spin.setValue(0)
        self.lasergrbl_black_clip_spin.hide()

        self.spot_removal_check = QCheckBox("斑点清除", self)
        self.spot_removal_check.setChecked(True)
        self.spot_removal_check.hide()
        self.vector_noise_label = QLabel("斑点面积", self)
        self.vector_noise_label.hide()
        self.vector_min_area = QSpinBox(self)
        self.vector_min_area.setObjectName("vectorMinArea")
        self.vector_min_area.setRange(1, 1000)
        self.vector_min_area.setValue(4)
        self.vector_min_area.hide()
        self.smoothing_check = QCheckBox("光滑", self)
        self.smoothing_check.setChecked(True)
        self.smoothing_check.hide()
        self.vector_simplify_label = QLabel("光滑强度", self)
        self.vector_simplify_label.hide()
        self.vector_simplify = QDoubleSpinBox(self)
        self.vector_simplify.setObjectName("vectorSimplify")
        self.vector_simplify.setRange(0.0, 5.0)
        self.vector_simplify.setSingleStep(0.1)
        self.vector_simplify.setDecimals(1)
        self.vector_simplify.setValue(1.0)
        self.vector_simplify.hide()

        self.optimize_paths_check = QCheckBox("优化路径", self)
        self.optimize_paths_check.setChecked(True)
        self.optimize_paths_check.hide()
        self.adaptive_quality_check = QCheckBox("自适应质量", self)
        self.adaptive_quality_check.setChecked(False)
        self.adaptive_quality_check.hide()
        self.downsample_check = QCheckBox("缩减取样", self)
        self.downsample_check.setChecked(False)
        self.downsample_check.hide()
        self.downsample_factor = QDoubleSpinBox(self)
        self.downsample_factor.setObjectName("downsampleFactor")
        self.downsample_factor.setRange(1.0, 4.0)
        self.downsample_factor.setSingleStep(0.5)
        self.downsample_factor.setDecimals(1)
        self.downsample_factor.setValue(2.0)
        self.downsample_factor.hide()
        self.vector_fill_combo = QComboBox(self)
        self.vector_fill_combo.setObjectName("vectorFillMode")
        self.vector_fill_combo.addItem("无填充", "none")
        self.vector_fill_combo.addItem("扫描填充", "scanline")
        self.vector_fill_combo.hide()

    def _build_hidden_target_parameter_state(self) -> None:
        self.laser_mode_combo = QComboBox(self)
        self.laser_mode_combo.setObjectName("laserMode")
        self.laser_mode_combo.addItem("M4 - Dynamic Power", "M4")
        self.laser_mode_combo.addItem("M3 - Constant Power", "M3")
        self.laser_mode_combo.hide()

        self.speed_spin = QSpinBox(self)
        self.speed_spin.setRange(100, 12_000)
        self.speed_spin.setValue(3000)
        self.speed_spin.hide()
        self.speed_slider = None
        self.power_min_spin = QSpinBox(self)
        self.power_min_spin.setRange(0, 1000)
        self.power_min_spin.setValue(0)
        self.power_min_spin.hide()
        self.power_min_slider = None
        self.power_spin = QSpinBox(self)
        self.power_spin.setRange(0, 1000)
        self.power_spin.setValue(1000)
        self.power_spin.hide()
        self.power_slider = None

        self.auto_size_check = QCheckBox("自动调整大小", self)
        self.auto_size_check.setChecked(False)
        self.auto_size_check.hide()
        self.dpi_spin = QSpinBox(self)
        self.dpi_spin.setObjectName("targetDpi")
        self.dpi_spin.setRange(50, 1200)
        self.dpi_spin.setValue(300)
        self.dpi_spin.hide()
        self.size_spin = QDoubleSpinBox(self)
        self.size_spin.setObjectName("markWidthMm")
        self.size_spin.setRange(0.0, 99.0)
        self.size_spin.setDecimals(1)
        self.size_spin.setSingleStep(1.0)
        self.size_spin.setValue(99.0)
        self.size_spin.hide()
        self.target_height_spin = QDoubleSpinBox(self)
        self.target_height_spin.setObjectName("markHeightMm")
        self.target_height_spin.setRange(0.0, 99.0)
        self.target_height_spin.setDecimals(1)
        self.target_height_spin.setSingleStep(1.0)
        self.target_height_spin.setValue(99.0)
        self.target_height_spin.hide()
        self.lock_aspect_check = QCheckBox("锁定宽高比", self)
        self.lock_aspect_check.setChecked(True)
        self.lock_aspect_check.hide()
        self.offset_x_spin = QDoubleSpinBox(self)
        self.offset_x_spin.setObjectName("offsetX")
        self.offset_x_spin.setRange(0.0, 99.0)
        self.offset_x_spin.setDecimals(1)
        self.offset_x_spin.setValue(0.0)
        self.offset_x_spin.hide()
        self.offset_y_spin = QDoubleSpinBox(self)
        self.offset_y_spin.setObjectName("offsetY")
        self.offset_y_spin.setRange(0.0, 99.0)
        self.offset_y_spin.setDecimals(1)
        self.offset_y_spin.setValue(0.0)
        self.offset_y_spin.hide()

    @staticmethod
    def _set_combo_data(combo: QComboBox, data: object) -> None:
        index = combo.findData(data)
        if index >= 0:
            combo.setCurrentIndex(index)

    @staticmethod
    def _lasergrbl_dialog_stylesheet() -> str:
        return """
            QDialog {
                background:#f8fafc;
                color:#0f172a;
            }
            QWidget#laserDialogLeft,
            QWidget#targetDialogBody {
                background:#f8fafc;
                color:#0f172a;
            }
            QGroupBox {
                background:#ffffff;
                border:1px solid #cbd5e1;
                border-radius:8px;
                margin-top:10px;
                padding:14px 10px 10px 10px;
                color:#0284c7;
                font-weight:700;
            }
            QGroupBox::title {
                subcontrol-origin:margin;
                subcontrol-position:top left;
                left:8px;
                padding:0 4px;
                color:#0284c7;
                background:#f8fafc;
            }
            QLabel {
                color:#334155;
                background:transparent;
            }
            QLabel:disabled,
            QCheckBox:disabled {
                color:#94a3b8;
            }
            QComboBox {
                background:#ffffff;
                color:#0f172a;
                border:1px solid #cbd5e1;
                border-radius:6px;
                padding:4px 6px;
                min-height:22px;
                selection-background-color:#0284c7;
            }
            QSpinBox,
            QDoubleSpinBox {
                background:#ffffff;
                color:#0f172a;
                border:1px solid #cbd5e1;
                border-radius:6px;
                padding:4px 20px 4px 6px;
                min-height:22px;
                selection-background-color:#0284c7;
            }
            QSpinBox::up-button, QDoubleSpinBox::up-button {
                subcontrol-origin: border;
                subcontrol-position: top right;
                width: 16px;
            }
            QSpinBox::down-button, QDoubleSpinBox::down-button {
                subcontrol-origin: border;
                subcontrol-position: bottom right;
                width: 16px;
            }
            QComboBox:disabled,
            QSpinBox:disabled,
            QDoubleSpinBox:disabled {
                background:#f1f5f9;
                color:#94a3b8;
                border-color:#dbe3ee;
            }
            QCheckBox {
                color:#334155;
                spacing:6px;
                background:transparent;
            }
            QCheckBox::indicator {
                width:14px;
                height:14px;
                border:1px solid #94a3b8;
                border-radius:3px;
                background:#f8fafc;
            }
            QCheckBox::indicator:checked {
                background:#0284c7;
                border-color:#0284c7;
            }
            QSlider::groove:horizontal {
                height:4px;
                background:#cbd5e1;
            }
            QSlider::sub-page:horizontal {
                background:#0284c7;
            }
            QSlider::handle:horizontal {
                background:#ffffff;
                border:1px solid #94a3b8;
                width:12px;
                margin:-5px 0;
                border-radius:6px;
            }
            QTabWidget::pane {
                border:1px solid #cbd5e1;
                background:#ffffff;
            }
            QTabBar::tab {
                background:#e2e8f0;
                color:#334155;
                padding:7px 14px;
                border:1px solid #cbd5e1;
                border-bottom:none;
            }
            QTabBar::tab:selected {
                background:#ffffff;
                color:#0284c7;
            }
            QPushButton {
                background:#ffffff;
                color:#0f172a;
                border:1px solid #cbd5e1;
                padding:7px 16px;
                border-radius:6px;
                min-width:64px;
            }
            QPushButton:hover {
                background:#f1f5f9;
            }
            QDialogButtonBox QPushButton {
                min-width:70px;
            }
        """

    def _open_image_parameter_dialog(self) -> None:
        dialog = QDialog(self)
        dialog.setWindowTitle("图像导入参数")
        dialog.setMinimumSize(900, 600)
        dialog.setStyleSheet(self._lasergrbl_dialog_stylesheet())
        layout = QVBoxLayout(dialog)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)
        body = QHBoxLayout()
        body.setSpacing(14)
        layout.addLayout(body, 1)
        left_panel = QWidget(dialog)
        left_panel.setObjectName("laserDialogLeft")
        left_panel.setFixedWidth(315)
        left_layout = QVBoxLayout(left_panel)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.setSpacing(8)
        body.addWidget(left_panel)

        preview_tabs = QTabWidget(dialog)
        preview_tabs.setObjectName("lasergrblPreviewTabs")
        preview_label = QLabel("未加载图像")
        preview_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        preview_label.setMinimumSize(420, 420)
        preview_label.setStyleSheet("background:#ffffff; color:#334155; border:1px solid #e2e8f0;")
        original_label = QLabel("未加载图像")
        original_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        original_label.setMinimumSize(420, 420)
        original_label.setStyleSheet("background:#ffffff; color:#334155; border:1px solid #e2e8f0;")
        preview_tabs.addTab(preview_label, "预览")
        preview_tabs.addTab(original_label, "原始图片")
        body.addWidget(preview_tabs, 1)

        def int_slider(value: int, minimum: int, maximum: int) -> tuple[QWidget, QSpinBox]:
            row = QWidget()
            row_layout = QHBoxLayout(row)
            row_layout.setContentsMargins(0, 0, 0, 0)
            row_layout.setSpacing(8)
            slider = QSlider(Qt.Horizontal)
            slider.setRange(minimum, maximum)
            slider.setValue(value)
            spin = QSpinBox()
            spin.setRange(minimum, maximum)
            spin.setValue(value)
            spin.setFixedWidth(80)
            slider.valueChanged.connect(spin.setValue)
            spin.valueChanged.connect(slider.setValue)
            row_layout.addWidget(slider, 1)
            row_layout.addWidget(spin)
            return row, spin

        params_group = QGroupBox("参数", dialog)
        params_form = QFormLayout(params_group)
        params_form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft)
        params_form.setHorizontalSpacing(14)
        params_form.setVerticalSpacing(8)
        left_layout.addWidget(params_group)

        tool_combo = QComboBox(dialog)
        for index in range(self.extract_mode.count()):
            tool_combo.addItem(self.extract_mode.itemText(index), self.extract_mode.itemData(index))
        self._set_combo_data(tool_combo, self.extract_mode.currentData())
        params_form.addRow("转换工具", tool_combo)

        resize_combo = QComboBox(dialog)
        resize_combo.addItem("Sharp (NearestNeighbor)", "nearest")
        resize_combo.addItem("Smooth (Bilinear)", "smooth")
        self._set_combo_data(resize_combo, self.resize_mode_combo.currentData())
        params_form.addRow("调整大小", resize_combo)

        row, threshold = int_slider(self.threshold_spin.value(), 0, 255)
        params_form.addRow("阈值", row)
        row, brightness = int_slider(self.lasergrbl_brightness_spin.value(), -100, 100)
        params_form.addRow("亮度", row)
        row, contrast = int_slider(self.lasergrbl_contrast_spin.value(), -100, 100)
        params_form.addRow("对比度", row)
        row, white_clip = int_slider(self.lasergrbl_white_clip_spin.value(), 0, 255)
        params_form.addRow("白色限制", row)
        row, black_clip = int_slider(self.lasergrbl_black_clip_spin.value(), 0, 255)
        params_form.addRow("黑色增强", row)

        vector_group = QGroupBox("矢量化：选项", dialog)
        vector_form = QFormLayout(vector_group)
        vector_form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft)
        vector_form.setHorizontalSpacing(14)
        vector_form.setVerticalSpacing(8)
        left_layout.addWidget(vector_group)
        spot_check = QCheckBox("启用")
        spot_check.setChecked(self.spot_removal_check.isChecked())
        spot_area = QSpinBox()
        spot_area.setRange(1, 1000)
        spot_area.setValue(self.vector_min_area.value())
        spot_area.setFixedWidth(80)
        spot_row = QWidget()
        spot_layout = QHBoxLayout(spot_row)
        spot_layout.setContentsMargins(0, 0, 0, 0)
        spot_layout.setSpacing(8)
        spot_layout.addWidget(spot_area)
        spot_layout.addWidget(spot_check)
        vector_form.addRow("斑点清除", spot_row)

        smooth_check = QCheckBox("启用")
        smooth_check.setChecked(self.smoothing_check.isChecked())
        smooth_value = QDoubleSpinBox()
        smooth_value.setRange(0.0, 5.0)
        smooth_value.setDecimals(1)
        smooth_value.setSingleStep(0.1)
        smooth_value.setValue(self.vector_simplify.value())
        smooth_value.setFixedWidth(80)
        smooth_row = QWidget()
        smooth_layout = QHBoxLayout(smooth_row)
        smooth_layout.setContentsMargins(0, 0, 0, 0)
        smooth_layout.setSpacing(8)
        smooth_layout.addWidget(smooth_value)
        smooth_layout.addWidget(smooth_check)
        vector_form.addRow("光滑", smooth_row)

        optimize_check = QCheckBox("优化路径")
        optimize_check.setChecked(self.optimize_paths_check.isChecked())
        adaptive_check = QCheckBox("自适应质量")
        adaptive_check.setChecked(self.adaptive_quality_check.isChecked())

        checkbox_row = QWidget()
        checkbox_layout = QHBoxLayout(checkbox_row)
        checkbox_layout.setContentsMargins(0, 0, 0, 0)
        checkbox_layout.setSpacing(12)
        checkbox_layout.addWidget(optimize_check)
        checkbox_layout.addWidget(adaptive_check)
        vector_form.addRow("", checkbox_row)

        downsample_check = QCheckBox("启用")
        downsample_check.setChecked(self.downsample_check.isChecked())
        downsample_value = QDoubleSpinBox()
        downsample_value.setRange(1.0, 4.0)
        downsample_value.setDecimals(1)
        downsample_value.setSingleStep(0.5)
        downsample_value.setValue(self.downsample_factor.value())
        downsample_value.setFixedWidth(80)
        downsample_row = QWidget()
        downsample_layout = QHBoxLayout(downsample_row)
        downsample_layout.setContentsMargins(0, 0, 0, 0)
        downsample_layout.setSpacing(8)
        downsample_layout.addWidget(downsample_value)
        downsample_layout.addWidget(downsample_check)
        vector_form.addRow("缩减取样", downsample_row)

        fill_combo = QComboBox(dialog)
        fill_combo.addItem("无填充", "none")
        fill_combo.addItem("扫描填充", "scanline")
        self._set_combo_data(fill_combo, self.vector_fill_combo.currentData())
        vector_form.addRow("填充", fill_combo)

        def update_dialog_vector_controls() -> None:
            mode = tool_combo.currentData()
            enabled = mode in ("vector", "lasergrbl_vector", "centerline")
            fill_enabled = mode in ("vector", "lasergrbl_vector")
            vector_group.setVisible(enabled)
            spot_check.setEnabled(enabled)
            spot_area.setEnabled(enabled)
            smooth_check.setEnabled(enabled)
            smooth_value.setEnabled(enabled)
            optimize_check.setEnabled(enabled)
            adaptive_check.setEnabled(enabled)
            downsample_check.setEnabled(enabled)
            downsample_value.setEnabled(enabled)
            fill_combo.setEnabled(fill_enabled)

        tool_combo.currentIndexChanged.connect(lambda *_: update_dialog_vector_controls())
        update_dialog_vector_controls()

        left_layout.addStretch(1)

        def render_preview() -> None:
            if self._source_image.isNull():
                return
            transform_mode = (
                Qt.FastTransformation
                if resize_combo.currentData() == "nearest"
                else Qt.SmoothTransformation
            )
            image = self._source_image.scaled(
                320, 320, Qt.KeepAspectRatio, transform_mode
            )
            mode = tool_combo.currentData()
            if (
                downsample_check.isChecked()
                and mode in ("vector", "lasergrbl_vector", "centerline")
                and downsample_value.value() > 1.0
            ):
                factor = downsample_value.value()
                image = image.scaled(
                    max(1, int(image.width() / factor)),
                    max(1, int(image.height() / factor)),
                    Qt.KeepAspectRatio,
                    Qt.FastTransformation,
                )
            rows = [
                [image.pixelColor(x, y).lightness() for x in range(image.width())]
                for y in range(image.height())
            ]
            tone_rows = apply_lasergrbl_tone(
                rows,
                brightness=brightness.value(),
                contrast=contrast.value(),
                white_clip=white_clip.value(),
                black_clip=black_clip.value(),
            )
            min_area = float(spot_area.value()) if spot_check.isChecked() else 1.0
            smoothing = smooth_value.value() if smooth_check.isChecked() else 0.0
            if mode == "dither":
                preview = self._build_mask_preview(
                    dither_rows_to_mask(tone_rows, threshold.value())
                )
            elif mode == "centerline":
                base = prepare_laser_mask(
                    tone_rows,
                    threshold.value(),
                    adaptive=adaptive_check.isChecked(),
                    min_area_px=min_area,
                )
                preview = self._build_mask_preview(centerline_mask(base))
            elif mode in ("vector", "lasergrbl_vector"):
                if mode == "lasergrbl_vector":
                    result = lasergrbl_style_vectorize(
                        rows,
                        LaserGrblVectorOptions(
                            threshold=threshold.value(),
                            brightness=brightness.value(),
                            contrast=contrast.value(),
                            white_clip=white_clip.value(),
                            black_clip=black_clip.value(),
                            spot_remove_px=min_area,
                            smoothing_px=smoothing,
                            optimize_paths=optimize_check.isChecked(),
                        ),
                    )
                    preview = self._build_vector_preview(result.rows, result.contours)
                else:
                    mask = prepare_laser_mask(
                        tone_rows,
                        threshold.value(),
                        adaptive=True,
                        edge_enhance=True,
                        min_area_px=min_area,
                    )
                    contours = simplify_vector_contours(
                        trace_vector_contours_from_mask(mask, min_area_px=min_area),
                        tolerance_px=smoothing,
                    )
                    preview = self._build_vector_preview(tone_rows, contours)
            else:
                mask = prepare_laser_mask(
                    tone_rows,
                    threshold.value(),
                    adaptive=mode == "scanline",
                    min_area_px=min_area,
                )
                preview = self._build_mask_preview(mask)

            preview_label.setPixmap(
                QPixmap.fromImage(preview).scaled(
                    preview_label.size(),
                    Qt.KeepAspectRatio,
                    Qt.SmoothTransformation,
                )
            )
            original_label.setPixmap(
                QPixmap.fromImage(self._source_image).scaled(
                    original_label.size(),
                    Qt.KeepAspectRatio,
                    Qt.SmoothTransformation,
                )
            )

        for widget in (
            resize_combo,
            threshold,
            brightness,
            contrast,
            white_clip,
            black_clip,
            tool_combo,
            spot_check,
            spot_area,
            smooth_check,
            smooth_value,
            optimize_check,
            adaptive_check,
            downsample_check,
            downsample_value,
            fill_combo,
        ):
            if isinstance(widget, QComboBox):
                widget.currentIndexChanged.connect(lambda *_: render_preview())
            elif isinstance(widget, (QSpinBox, QDoubleSpinBox)):
                widget.valueChanged.connect(lambda *_: render_preview())
            elif isinstance(widget, QCheckBox):
                widget.toggled.connect(lambda *_: render_preview())
        render_preview()

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        layout.addWidget(buttons)

        def accept() -> None:
            self._set_combo_data(self.resize_mode_combo, resize_combo.currentData())
            self.extract_mode.setCurrentIndex(self.extract_mode.findData(tool_combo.currentData()))
            self.threshold_spin.setValue(threshold.value())
            self.lasergrbl_brightness_spin.setValue(brightness.value())
            self.lasergrbl_contrast_spin.setValue(contrast.value())
            self.lasergrbl_white_clip_spin.setValue(white_clip.value())
            self.lasergrbl_black_clip_spin.setValue(black_clip.value())
            self.spot_removal_check.setChecked(spot_check.isChecked())
            self.vector_min_area.setValue(spot_area.value())
            self.smoothing_check.setChecked(smooth_check.isChecked())
            self.vector_simplify.setValue(smooth_value.value())
            self.optimize_paths_check.setChecked(optimize_check.isChecked())
            self.adaptive_quality_check.setChecked(adaptive_check.isChecked())
            self.downsample_check.setChecked(downsample_check.isChecked())
            self.downsample_factor.setValue(downsample_value.value())
            self._set_combo_data(self.vector_fill_combo, fill_combo.currentData())
            dialog.accept()

        buttons.accepted.connect(accept)
        buttons.rejected.connect(dialog.reject)
        dialog.exec()

    def _open_target_parameter_dialog(self) -> None:
        dialog = QDialog(self)
        dialog.setWindowTitle("目标图像与激光")
        dialog.setMinimumSize(560, 500)
        dialog.setStyleSheet(self._lasergrbl_dialog_stylesheet())
        layout = QVBoxLayout(dialog)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)
        body = QWidget(dialog)
        body.setObjectName("targetDialogBody")
        body_layout = QVBoxLayout(body)
        body_layout.setContentsMargins(0, 0, 0, 0)
        body_layout.setSpacing(10)
        layout.addWidget(body, 1)
        intensity_group = QGroupBox("强度", dialog)
        intensity_form = QFormLayout(intensity_group)
        intensity_form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft)
        intensity_form.setHorizontalSpacing(16)
        intensity_form.setVerticalSpacing(8)
        body_layout.addWidget(intensity_group)

        laser_group = QGroupBox("激光选项", dialog)
        laser_form = QFormLayout(laser_group)
        laser_form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft)
        laser_form.setHorizontalSpacing(16)
        laser_form.setVerticalSpacing(8)
        body_layout.addWidget(laser_group)

        size_group = QGroupBox("图像大小和位置 [mm]", dialog)
        size_form = QFormLayout(size_group)
        size_form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft)
        size_form.setHorizontalSpacing(16)
        size_form.setVerticalSpacing(8)
        body_layout.addWidget(size_group)

        laser_mode = QComboBox(dialog)
        laser_mode.addItem("M4 - Dynamic Power", "M4")
        laser_mode.addItem("M3 - Constant Power", "M3")
        self._set_combo_data(laser_mode, self.laser_mode_combo.currentData())
        laser_form.addRow("激光模式", laser_mode)

        speed = QSpinBox(dialog)
        speed.setRange(100, 12_000)
        speed.setValue(self.speed_spin.value())
        speed.setFixedWidth(140)
        intensity_form.addRow("边界速度 mm/min", speed)
        min_s = QSpinBox(dialog)
        min_s.setRange(0, 1000)
        min_s.setValue(self.power_min_spin.value())
        min_s.setFixedWidth(140)
        laser_form.addRow("最小 S 值", min_s)
        max_s = QSpinBox(dialog)
        max_s.setRange(0, 1000)
        max_s.setValue(self.power_spin.value())
        max_s.setFixedWidth(140)
        laser_form.addRow("最大 S 值", max_s)

        auto_size = QCheckBox("自动调整大小")
        auto_size.setChecked(self.auto_size_check.isChecked())
        dpi = QSpinBox(dialog)
        dpi.setRange(50, 1200)
        dpi.setValue(self.dpi_spin.value())
        dpi.setFixedWidth(110)
        dpi.setPrefix("DPI: ")
        auto_row = QWidget(size_group)
        auto_layout = QHBoxLayout(auto_row)
        auto_layout.setContentsMargins(0, 0, 0, 0)
        auto_layout.setSpacing(12)
        auto_layout.addWidget(dpi)
        auto_layout.addWidget(auto_size)
        size_form.addRow("自动大小", auto_row)

        width = QDoubleSpinBox(dialog)
        width.setRange(0.0, 99.0)
        width.setDecimals(1)
        width.setSingleStep(1.0)
        width.setValue(self.size_spin.value())
        width.setFixedWidth(110)
        width.setPrefix("W: ")
        height = QDoubleSpinBox(dialog)
        height.setRange(0.0, 99.0)
        height.setDecimals(1)
        height.setSingleStep(1.0)
        height.setValue(self.target_height_spin.value())
        height.setFixedWidth(110)
        height.setPrefix("H: ")
        lock = QCheckBox("锁定")
        lock.setChecked(self.lock_aspect_check.isChecked())
        size_row = QWidget(size_group)
        size_layout = QHBoxLayout(size_row)
        size_layout.setContentsMargins(0, 0, 0, 0)
        size_layout.setSpacing(12)
        size_layout.addWidget(width)
        size_layout.addWidget(height)
        size_layout.addWidget(lock)
        size_form.addRow("大小", size_row)

        offset_x = QDoubleSpinBox(dialog)
        offset_x.setRange(0.0, 99.0)
        offset_x.setDecimals(1)
        offset_x.setValue(self.offset_x_spin.value())
        offset_x.setFixedWidth(110)
        offset_x.setPrefix("X: ")
        offset_y = QDoubleSpinBox(dialog)
        offset_y.setRange(0.0, 99.0)
        offset_y.setDecimals(1)
        offset_y.setValue(self.offset_y_spin.value())
        offset_y.setFixedWidth(110)
        offset_y.setPrefix("Y: ")
        offset_row = QWidget(size_group)
        offset_layout = QHBoxLayout(offset_row)
        offset_layout.setContentsMargins(0, 0, 0, 0)
        offset_layout.setSpacing(12)
        offset_layout.addWidget(offset_x)
        offset_layout.addWidget(offset_y)
        offset_layout.addStretch(1)
        size_form.addRow("偏移", offset_row)
        body_layout.addStretch(1)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        layout.addWidget(buttons)

        def accept() -> None:
            self._set_combo_data(self.laser_mode_combo, laser_mode.currentData())
            self.speed_spin.setValue(speed.value())
            self.power_min_spin.setValue(min_s.value())
            self.power_spin.setValue(max_s.value())
            self.auto_size_check.setChecked(auto_size.isChecked())
            self.dpi_spin.setValue(dpi.value())
            self.size_spin.setValue(width.value())
            self.target_height_spin.setValue(height.value())
            self.lock_aspect_check.setChecked(lock.isChecked())
            self.offset_x_spin.setValue(offset_x.value())
            self.offset_y_spin.setValue(offset_y.value())
            self._apply_auto_size_if_needed()
            dialog.accept()

        buttons.accepted.connect(accept)
        buttons.rejected.connect(dialog.reject)
        dialog.exec()

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
        self.highlighter = GcodeHighlighter(self.editor.document())
        editor_layout.addWidget(self.editor, 1)
        layout.addWidget(editor_card, 1)
        return tab

    def _update_vector_controls(self) -> None:
        mode = self.extract_mode.currentData()
        enabled = mode in ("vector", "lasergrbl_vector", "centerline")
        fill_enabled = mode in ("vector", "lasergrbl_vector")
        for widget in (
            self.vector_simplify_label,
            self.vector_simplify,
            self.vector_noise_label,
            self.vector_min_area,
            self.spot_removal_check,
            self.smoothing_check,
            self.optimize_paths_check,
            self.downsample_check,
            self.downsample_factor,
            self.adaptive_quality_check,
        ):
            widget.setEnabled(enabled)
        self.vector_fill_combo.setEnabled(fill_enabled)

    def _sync_locked_height(self, value: float) -> None:
        if not self.lock_aspect_check.isChecked():
            return
        self.target_height_spin.blockSignals(True)
        self.target_height_spin.setValue(value)
        self.target_height_spin.blockSignals(False)

    def _apply_auto_size_if_needed(self) -> None:
        if not self.auto_size_check.isChecked() or self._source_image.isNull():
            return
        dpi = max(1, self.dpi_spin.value())
        width_mm = min(99.0, self._source_image.width() / dpi * 25.4)
        height_mm = min(99.0, self._source_image.height() / dpi * 25.4)
        self.size_spin.setValue(round(width_mm, 1))
        self.target_height_spin.setValue(round(height_mm, 1))

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
        self._apply_auto_size_if_needed()
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
        if mode in ("scanline", "dither"):
            suffix = SCANLINE_PROMPT_SUFFIX
        elif mode in ("vector", "lasergrbl_vector", "centerline", "passthrough"):
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
        transform_mode = (
            Qt.FastTransformation
            if self.resize_mode_combo.currentData() == "nearest"
            else Qt.SmoothTransformation
        )
        image = self._source_image.scaled(
            320,
            320,
            Qt.KeepAspectRatio,
            transform_mode,
        )
        mode = self.extract_mode.currentData()
        if (
            self.downsample_check.isChecked()
            and mode in ("vector", "lasergrbl_vector", "centerline")
            and self.downsample_factor.value() > 1.0
        ):
            factor = self.downsample_factor.value()
            image = image.scaled(
                max(1, int(image.width() / factor)),
                max(1, int(image.height() / factor)),
                Qt.KeepAspectRatio,
                Qt.FastTransformation,
            )
        rows = [
            [image.pixelColor(x, y).lightness() for x in range(image.width())]
            for y in range(image.height())
        ]
        tone_rows = apply_lasergrbl_tone(
            rows,
            brightness=self.lasergrbl_brightness_spin.value(),
            contrast=self.lasergrbl_contrast_spin.value(),
            white_clip=self.lasergrbl_white_clip_spin.value(),
            black_clip=self.lasergrbl_black_clip_spin.value(),
        )
        target_width_mm = float(self.size_spin.value())
        target_height_mm = float(self.target_height_spin.value())
        offset_x = float(self.offset_x_spin.value())
        offset_y = float(self.offset_y_spin.value())
        laser_mode = str(self.laser_mode_combo.currentData())
        mark_power = max(self.power_min_spin.value(), self.power_spin.value())
        min_area = float(self.vector_min_area.value()) if self.spot_removal_check.isChecked() else 1.0
        smoothing = self.vector_simplify.value() if self.smoothing_check.isChecked() else 0.0
        path_count = 0
        point_count = 0

        def merge_jobs(first: str, second: str) -> str:
            first_lines = first.strip().splitlines()
            second_lines = second.strip().splitlines()
            if first_lines and first_lines[-1] == "M5":
                first_lines.pop()
            if second_lines and second_lines[0].startswith(("M3 ", "M4 ")):
                second_lines = second_lines[1:]
            return "\n".join(first_lines + second_lines) + "\n"

        labels = {
            "scanline": ("线到线跟踪", "scanline"),
            "dither": ("1bit B/W抖动", "dither"),
            "lasergrbl_vector": ("LaserGRBL风格矢量", "lasergrbl_vector"),
            "centerline": ("中心线", "centerline"),
            "passthrough": ("直通", "passthrough"),
            "vector": ("轮廓矢量", "vector"),
        }
        mode_label, suffix = labels.get(mode, labels["scanline"])

        if target_width_mm == 0 or target_height_mm == 0:
            text = f"{laser_mode} S0\nM5\n"
            self._converted_image = QImage(
                image.width(), image.height(), QImage.Format.Format_RGB32
            )
            self._converted_image.fill(QColor("white"))
            self._converted_status = (
                f"{mode_label} · 0×0 mm · 安全空任务，不生成移动或开光路径 · "
                f"{len(text.encode('utf-8'))} bytes"
            )
            source = Path(self._image_path).stem if self._image_path else "generated_image"
            self.set_content(f"{source}.{suffix}.generated.gcode", text.encode("utf-8"))
            self.btn_preview_converted.setEnabled(True)
            self._set_preview_mode("converted")
            return

        if mode in ("vector", "lasergrbl_vector"):
            if mode == "lasergrbl_vector":
                result = lasergrbl_style_vectorize(
                    rows,
                    LaserGrblVectorOptions(
                        threshold=self.threshold_spin.value(),
                        brightness=self.lasergrbl_brightness_spin.value(),
                        contrast=self.lasergrbl_contrast_spin.value(),
                        white_clip=self.lasergrbl_white_clip_spin.value(),
                        black_clip=self.lasergrbl_black_clip_spin.value(),
                        spot_remove_px=min_area,
                        smoothing_px=smoothing,
                        optimize_paths=self.optimize_paths_check.isChecked(),
                    ),
                )
                contours = result.contours
                mask = result.mask
                preview_rows = result.rows
                mode_label = "LaserGRBL风格矢量"
                algorithm = (
                    f"Potrace风格阈值 · 亮度 {self.lasergrbl_brightness_spin.value()} · "
                    f"对比 {self.lasergrbl_contrast_spin.value()} · "
                    f"白场 {self.lasergrbl_white_clip_spin.value()} · "
                    f"黑场 {self.lasergrbl_black_clip_spin.value()}"
                )
                suffix = "lasergrbl_vector"
            else:
                mask = prepare_laser_mask(
                    tone_rows,
                    self.threshold_spin.value(),
                    adaptive=True,
                    edge_enhance=True,
                    min_area_px=min_area,
                )
                contours = trace_vector_contours_from_mask(
                    mask, min_area_px=min_area,
                )
                contours = simplify_vector_contours(
                    contours, tolerance_px=smoothing,
                )
                preview_rows = tone_rows
                mode_label = "轮廓矢量"
                algorithm = "边缘增强"
                suffix = "vector"
            text = vector_contours_to_gcode(
                contours,
                image.width(),
                image.height(),
                target_width_mm,
                self.speed_spin.value(),
                height_mm=target_height_mm,
                power_s=mark_power,
                laser_mode=laser_mode,
                x_offset_mm=offset_x,
                y_offset_mm=offset_y,
                optimize_paths=self.optimize_paths_check.isChecked(),
            )
            if self.vector_fill_combo.currentData() == "scanline":
                fill_text = raster_mask_to_gcode(
                    mask,
                    target_width_mm,
                    self.speed_spin.value(),
                    height_mm=target_height_mm,
                    power_s=mark_power,
                    laser_mode=laser_mode,
                    x_offset_mm=offset_x,
                    y_offset_mm=offset_y,
                )
                text = merge_jobs(fill_text, text)
                algorithm += " · 扫描填充"
            self._converted_image = self._build_vector_preview(preview_rows, contours)
            path_count = len(contours)
            point_count = sum(max(0, len(contour) - 1) for contour in contours)
        elif mode == "dither":
            mask = dither_rows_to_mask(tone_rows, self.threshold_spin.value())
            text = raster_mask_to_gcode(
                mask,
                target_width_mm,
                self.speed_spin.value(),
                height_mm=target_height_mm,
                power_s=mark_power,
                laser_mode=laser_mode,
                x_offset_mm=offset_x,
                y_offset_mm=offset_y,
            )
            self._converted_image = self._build_mask_preview(mask)
            algorithm = "Floyd-Steinberg 1bit"
        elif mode == "centerline":
            base_mask = prepare_laser_mask(
                tone_rows,
                self.threshold_spin.value(),
                adaptive=self.adaptive_quality_check.isChecked(),
                edge_enhance=False,
                min_area_px=min_area,
            )
            mask = centerline_mask(base_mask)
            text = raster_mask_to_gcode(
                mask,
                target_width_mm,
                self.speed_spin.value(),
                height_mm=target_height_mm,
                power_s=mark_power,
                laser_mode=laser_mode,
                x_offset_mm=offset_x,
                y_offset_mm=offset_y,
            )
            self._converted_image = self._build_mask_preview(mask)
            algorithm = "二值细化中心线"
        elif mode == "passthrough":
            mask = prepare_laser_mask(
                tone_rows,
                self.threshold_spin.value(),
                adaptive=False,
                edge_enhance=False,
                min_area_px=min_area,
            )
            text = raster_mask_to_gcode(
                mask,
                target_width_mm,
                self.speed_spin.value(),
                height_mm=target_height_mm,
                power_s=mark_power,
                laser_mode=laser_mode,
                x_offset_mm=offset_x,
                y_offset_mm=offset_y,
            )
            self._converted_image = self._build_mask_preview(mask)
            algorithm = "直接阈值"
        else:
            mask = prepare_laser_mask(
                tone_rows,
                self.threshold_spin.value(),
                adaptive=True,
                edge_enhance=False,
                min_area_px=min_area,
            )
            text = raster_mask_to_gcode(
                mask,
                target_width_mm,
                self.speed_spin.value(),
                height_mm=target_height_mm,
                power_s=mark_power,
                laser_mode=laser_mode,
                x_offset_mm=offset_x,
                y_offset_mm=offset_y,
            )
            self._converted_image = self._build_mask_preview(mask)
            algorithm = "线到线 · 自适应阈值"
        step_mm = min(
            target_width_mm / image.width(),
            target_height_mm / image.height(),
        )
        actual_width = image.width() * step_mm
        actual_height = image.height() * step_mm
        output_bytes = len(text.encode("utf-8"))
        vector_stats = (
            f" · 路径 {path_count} · 节点 {point_count}"
            if mode in ("vector", "lasergrbl_vector")
            else ""
        )
        size_warning = " · 超过100KiB上传上限" if output_bytes > RX_JOB_MAX_BYTES else ""
        self._converted_status = (
            f"{mode_label} · {algorithm} · 图形 {actual_width:.1f}×{actual_height:.1f} mm · "
            f"X {offset_x:.1f}-{offset_x + actual_width:.1f} · "
            f"Y {offset_y:.1f}-{offset_y + actual_height:.1f}\n"
            f"阈值 {self.threshold_spin.value()} · {laser_mode} · "
            f"S{self.power_min_spin.value()}-{mark_power} · S{mark_power}"
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

    def dragEnterEvent(self, event) -> None:
        if event.mimeData().hasUrls():
            for url in event.mimeData().urls():
                path = url.toLocalFile()
                if path.lower().endswith((".png", ".jpg", ".jpeg", ".bmp", ".webp")):
                    event.acceptProposedAction()
                    return
        event.ignore()

    def dropEvent(self, event) -> None:
        urls = event.mimeData().urls()
        if urls:
            path = urls[0].toLocalFile()
            image = QImage(path)
            if not image.isNull():
                self.set_image(path, image)
                event.acceptProposedAction()

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


class GcodeHighlighter(QSyntaxHighlighter):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.rules = []

        g_format = QTextCharFormat()
        g_format.setForeground(QColor("#10b981"))
        g_format.setFontWeight(QFont.Weight.Bold)
        self.rules.append((QRegularExpression(r"\bG\d+\b"), g_format))

        m_format = QTextCharFormat()
        m_format.setForeground(QColor("#0284c7"))
        m_format.setFontWeight(QFont.Weight.Bold)
        self.rules.append((QRegularExpression(r"\bM\d+\b"), m_format))

        axis_format = QTextCharFormat()
        axis_format.setForeground(QColor("#f97316"))
        self.rules.append((QRegularExpression(r"\b[XYZIJ][-+]?\d*\.?\d+\b"), axis_format))

        param_format = QTextCharFormat()
        param_format.setForeground(QColor("#8b5cf6"))
        self.rules.append((QRegularExpression(r"\b[FS]\d*\.?\d+\b"), param_format))

        comment_format = QTextCharFormat()
        comment_format.setForeground(QColor("#94a3b8"))
        self.rules.append((QRegularExpression(r";.*"), comment_format))

    def highlightBlock(self, text):
        for pattern, fmt in self.rules:
            expression = QRegularExpression(pattern)
            match_iterator = expression.globalMatch(text)
            while match_iterator.hasNext():
                match = match_iterator.next()
                self.setFormat(match.capturedStart(), match.capturedLength(), fmt)
