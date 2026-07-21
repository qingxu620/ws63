from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
from threading import Event
from time import monotonic
from typing import Callable

from PySide6.QtCore import (
    QLineF,
    QObject,
    QRect,
    QRunnable,
    QRegularExpression,
    QThreadPool,
    QTimer,
    Qt,
    Signal,
)
from PySide6.QtGui import QColor, QImage, QPainter, QPen, QPixmap, QSyntaxHighlighter, QTextCharFormat, QFont
from PySide6.QtWidgets import (
    QAbstractSpinBox,
    QApplication,
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
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSlider,
    QSpinBox,
    QTabWidget,
    QPlainTextEdit,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from app.image_gcode import (
    Contour,
    LaserGrblVectorOptions,
    apply_lasergrbl_tone,
    centerline_mask,
    centerline_paths_to_gcode,
    dither_rows_to_mask,
    grayscale_channel_weights,
    grayscale_power_rows,
    grayscale_rows_to_gcode,
    lasergrbl_style_vectorize,
    prepare_laser_mask,
    simplify_vector_contours,
    trace_centerline_paths,
    trace_vector_contours_from_mask,
    vector_contours_to_gcode,
)
from app.image_crop import (
    auto_crop_white_background,
    flip_image,
    rotate_image_quarter_turns,
)
from app.gcode_transform import (
    GcodeTransformOptions,
    GcodeTransformResult,
    GcodeTransformStats,
    analyze_gcode,
    transform_gcode,
)
from app.gcode_preview import GcodePreviewResult, render_gcode_preview
from app.paths import generated_images_dir
from ui.widgets.crop_image_widget import CropImageWidget


OUTLINE_SCAN_FEED_MM_MIN = 10000
OUTLINE_SCAN_POWER_S = 80
OUTLINE_SCAN_LOOPS = 2
DEFAULT_SCAN_QUALITY_LINES_MM = 10.0
MAX_SCAN_QUALITY_LINES_MM = 50.0
DIALOG_PREVIEW_MAX_PX = 512
DIALOG_PREVIEW_DEBOUNCE_MS = 150
EDITOR_HIGHLIGHT_MAX_BYTES = 512 * 1024
EDITOR_EAGER_LOAD_MAX_BYTES = 2 * 1024 * 1024
EDITOR_PREVIEW_MAX_BYTES = 256 * 1024
RASTER_EXTRACTION_MODES = ("scanline", "dither", "centerline", "passthrough")
GRAYSCALE_FORMULA_LABELS = (
    ("感知亮度（推荐）", "optical_correct"),
    ("简单平均", "simple_average"),
    ("加权平均", "weight_average"),
    ("自定义 RGB", "custom"),
)
DITHER_ALGORITHM_LABELS = (
    ("Floyd–Steinberg", "floyd_steinberg"),
    ("Atkinson", "atkinson"),
    ("Burkes", "burks"),
    ("Jarvis–Judice–Ninke", "jarvis"),
    ("Random（固定种子）", "random"),
    ("Sierra 2-row", "sierra2"),
    ("Sierra 3-row", "sierra3"),
    ("Sierra Lite", "sierra_lite"),
    ("Stucki", "stucki"),
)
SCAN_DIRECTION_LABELS = (
    ("水平（X）", "horizontal"),
    ("垂直（Y）", "vertical"),
    ("45° 对角线", "diagonal"),
)
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


class _PreviewTaskSignals(QObject):
    ready = Signal(int, QImage)
    failed = Signal(int, str)


class _PreviewTask(QRunnable):
    def __init__(self, serial: int, compute: Callable[[], QImage]) -> None:
        super().__init__()
        self.serial = serial
        self.compute = compute
        self.signals = _PreviewTaskSignals()

    def run(self) -> None:
        try:
            preview = self.compute()
        except Exception as exc:
            self.signals.failed.emit(self.serial, str(exc))
            return
        self.signals.ready.emit(self.serial, preview)


@dataclass(frozen=True)
class _RasterConversionRequest:
    source_image: QImage
    image_path: str
    mode: str
    smooth_resize: bool
    scan_quality: float
    target_width_mm: float
    target_height_mm: float
    lock_aspect: bool
    offset_x: float
    offset_y: float
    grayscale_formula: str
    red_weight: float
    green_weight: float
    blue_weight: float
    invert: bool
    brightness: int
    contrast: int
    white_clip: int
    black_clip: int
    laser_mode: str
    min_power: int
    mark_power: int
    threshold: int
    threshold_enabled: bool
    raster_pwm: bool
    auto_line_art: bool
    scan_direction: str
    scan_direction_label: str
    unidirectional: bool
    rapid_white: bool
    dither_algorithm: str
    dither_label: str
    mark_speed: int
    fill_speed: int
    min_area: float
    adaptive_quality: bool
    optimize_paths: bool


@dataclass(frozen=True)
class _ConversionResult:
    path: str
    data: bytes
    preview: QImage
    status: str


class _ConversionCancelled(RuntimeError):
    pass


class _ConversionTaskSignals(QObject):
    ready = Signal(int, object)
    failed = Signal(int, str)
    progress = Signal(int, str)
    finished = Signal(object)


class _ConversionTask(QRunnable):
    def __init__(
        self,
        serial: int,
        request: _RasterConversionRequest,
        compute: Callable[..., _ConversionResult],
    ) -> None:
        super().__init__()
        self.serial = serial
        self.request = request
        self.compute = compute
        self.cancelled = Event()
        self.signals = _ConversionTaskSignals()

    def cancel(self) -> None:
        self.cancelled.set()

    def run(self) -> None:
        def check_cancelled() -> None:
            if self.cancelled.is_set():
                raise _ConversionCancelled()

        try:
            result = self.compute(
                self.request,
                check_cancelled=check_cancelled,
                progress=lambda message: self.signals.progress.emit(
                    self.serial, message
                ),
            )
        except _ConversionCancelled:
            pass
        except Exception as exc:
            self.signals.failed.emit(self.serial, str(exc))
        else:
            if not self.cancelled.is_set():
                self.signals.ready.emit(self.serial, result)
        finally:
            self.signals.finished.emit(self)


class _GcodeAdjustmentCancelled(RuntimeError):
    pass


class _GcodeAdjustmentTaskSignals(QObject):
    ready = Signal(int, str, object)
    failed = Signal(int, str)
    finished = Signal(object)


class _GcodeAdjustmentTask(QRunnable):
    def __init__(
        self,
        serial: int,
        path: str,
        source_text: str,
        options: GcodeTransformOptions,
    ) -> None:
        super().__init__()
        self.serial = serial
        self.path = path
        self.source_text = source_text
        self.options = options
        self.cancelled = Event()
        self.signals = _GcodeAdjustmentTaskSignals()

    def cancel(self) -> None:
        self.cancelled.set()

    def run(self) -> None:
        def check_cancelled() -> None:
            if self.cancelled.is_set():
                raise _GcodeAdjustmentCancelled()

        try:
            result = transform_gcode(
                self.source_text,
                self.options,
                cancel_check=check_cancelled,
            )
        except _GcodeAdjustmentCancelled:
            pass
        except Exception as exc:
            self.signals.failed.emit(self.serial, str(exc))
        else:
            if not self.cancelled.is_set():
                self.signals.ready.emit(self.serial, self.path, result)
        finally:
            self.signals.finished.emit(self)


class _GcodeAnalysisTaskSignals(QObject):
    ready = Signal(int, str, str, object)
    failed = Signal(int, str)
    finished = Signal(object)


class _GcodeAnalysisTask(QRunnable):
    def __init__(self, serial: int, path: str, source_text: str) -> None:
        super().__init__()
        self.serial = serial
        self.path = path
        self.source_text = source_text
        self.cancelled = Event()
        self.signals = _GcodeAnalysisTaskSignals()

    def cancel(self) -> None:
        self.cancelled.set()

    def run(self) -> None:
        def check_cancelled() -> None:
            if self.cancelled.is_set():
                raise _GcodeAdjustmentCancelled()

        try:
            stats = analyze_gcode(
                self.source_text,
                cancel_check=check_cancelled,
            )
        except _GcodeAdjustmentCancelled:
            pass
        except Exception as exc:
            self.signals.failed.emit(self.serial, str(exc))
        else:
            if not self.cancelled.is_set():
                self.signals.ready.emit(
                    self.serial,
                    self.path,
                    self.source_text,
                    stats,
                )
        finally:
            self.signals.finished.emit(self)


class _GcodePreviewCancelled(RuntimeError):
    pass


class _GcodePreviewTaskSignals(QObject):
    ready = Signal(int, str, int, object)
    failed = Signal(int, str)
    finished = Signal(object)


class _GcodePreviewTask(QRunnable):
    def __init__(self, serial: int, path: str, data: bytes) -> None:
        super().__init__()
        self.serial = serial
        self.path = path
        self.data = bytes(data)
        self.cancelled = Event()
        self.signals = _GcodePreviewTaskSignals()

    def cancel(self) -> None:
        self.cancelled.set()

    def run(self) -> None:
        def check_cancelled() -> None:
            if self.cancelled.is_set():
                raise _GcodePreviewCancelled()

        try:
            result = render_gcode_preview(
                self.data.decode("utf-8", errors="replace"),
                cancel_check=check_cancelled,
            )
        except _GcodePreviewCancelled:
            pass
        except Exception as exc:
            self.signals.failed.emit(self.serial, str(exc))
        else:
            if not self.cancelled.is_set():
                self.signals.ready.emit(
                    self.serial,
                    self.path,
                    len(self.data),
                    result,
                )
        finally:
            self.signals.finished.emit(self)


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
    ai_generation_cancel_requested = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._path = ""
        self._gcode_data = b""
        self._editor_has_full_content = True
        self._image_path = ""
        self._original_image = QImage()
        self._source_image = QImage()
        self._converted_image = QImage()
        self._preview_mode = "original"
        self._is_generating = False
        self._image_preview_pool = QThreadPool(self)
        self._image_preview_pool.setMaxThreadCount(1)
        self._conversion_pool = QThreadPool(self)
        self._conversion_pool.setMaxThreadCount(1)
        self._conversion_serial = 0
        self._conversion_task: _ConversionTask | None = None
        self._conversion_tasks: set[_ConversionTask] = set()
        self._conversion_started_at = 0.0
        self._conversion_phase = ""
        self._conversion_status_timer = QTimer(self)
        self._conversion_status_timer.setInterval(125)
        self._conversion_status_timer.timeout.connect(
            self._refresh_conversion_progress_status
        )
        self._gcode_adjust_pool = QThreadPool(self)
        self._gcode_adjust_pool.setMaxThreadCount(1)
        self._gcode_adjust_serial = 0
        self._gcode_adjust_task: _GcodeAdjustmentTask | _GcodeAnalysisTask | None = None
        self._gcode_adjust_tasks: set[_GcodeAdjustmentTask | _GcodeAnalysisTask] = set()
        self._gcode_adjust_started_at = 0.0
        self._gcode_adjust_phase = ""
        self._gcode_adjust_timer = QTimer(self)
        self._gcode_adjust_timer.setInterval(250)
        self._gcode_adjust_timer.timeout.connect(self._refresh_gcode_adjust_status)
        self._gcode_preview_pool = QThreadPool(self)
        self._gcode_preview_pool.setMaxThreadCount(1)
        self._gcode_preview_serial = 0
        self._gcode_preview_task: _GcodePreviewTask | None = None
        self._gcode_preview_tasks: set[_GcodePreviewTask] = set()
        self._showing_gcode_preview = False
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
        default_output_dir = generated_images_dir()
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
        self.btn_generate.clicked.connect(self._handle_ai_generation_action)
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
        self.extract_mode.addItem("LaserGRBL 风格轮廓（非 Potrace）", "lasergrbl_vector")
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
        self.target_height_spin.valueChanged.connect(self._sync_locked_width)
        self.lock_aspect_check.toggled.connect(self._refresh_target_size_for_source)
        self.auto_size_check.toggled.connect(self._apply_auto_size_if_needed)
        self.dpi_spin.valueChanged.connect(self._apply_auto_size_if_needed)
        self.offset_x_spin.valueChanged.connect(self._apply_auto_size_if_needed)
        self.offset_y_spin.valueChanged.connect(self._apply_auto_size_if_needed)

        self.extract_mode.currentIndexChanged.connect(self._update_vector_controls)
        self.vector_fill_combo.currentIndexChanged.connect(self._update_vector_controls)
        self._update_vector_controls()
        self.btn_convert = QPushButton("转换为 G-code")
        self.btn_convert.setObjectName("btnAccent")
        self.btn_convert.clicked.connect(self._handle_conversion_action)
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
        self.preview_label.setStyleSheet(
            "color:#94a3b8; font-size:14px; border:none; "
            "background:#f1f5f9; border-radius:8px;"
        )
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
        self.scan_quality_spin = QDoubleSpinBox(self)
        self.scan_quality_spin.setObjectName("scanQualityLinesPerMm")
        self.scan_quality_spin.setRange(1.0, MAX_SCAN_QUALITY_LINES_MM)
        self.scan_quality_spin.setDecimals(1)
        self.scan_quality_spin.setSingleStep(1.0)
        self.scan_quality_spin.setValue(DEFAULT_SCAN_QUALITY_LINES_MM)
        self.scan_quality_spin.hide()

        self.resize_mode_combo = QComboBox(self)
        self.resize_mode_combo.setObjectName("resizeMode")
        self.resize_mode_combo.addItem("Sharp (NearestNeighbor)", "nearest")
        self.resize_mode_combo.addItem("Smooth (Bilinear)", "smooth")
        self.resize_mode_combo.hide()

        self.grayscale_formula_combo = QComboBox(self)
        self.grayscale_formula_combo.setObjectName("grayscaleFormula")
        for label, data in GRAYSCALE_FORMULA_LABELS:
            self.grayscale_formula_combo.addItem(label, data)
        self.grayscale_formula_combo.hide()
        self.grayscale_red_spin = QSpinBox(self)
        self.grayscale_red_spin.setObjectName("grayscaleRedWeight")
        self.grayscale_red_spin.setRange(0, 300)
        self.grayscale_red_spin.setValue(100)
        self.grayscale_red_spin.hide()
        self.grayscale_green_spin = QSpinBox(self)
        self.grayscale_green_spin.setObjectName("grayscaleGreenWeight")
        self.grayscale_green_spin.setRange(0, 300)
        self.grayscale_green_spin.setValue(100)
        self.grayscale_green_spin.hide()
        self.grayscale_blue_spin = QSpinBox(self)
        self.grayscale_blue_spin.setObjectName("grayscaleBlueWeight")
        self.grayscale_blue_spin.setRange(0, 300)
        self.grayscale_blue_spin.setValue(100)
        self.grayscale_blue_spin.hide()
        self.invert_image_check = QCheckBox("反色", self)
        self.invert_image_check.setObjectName("invertImage")
        self.invert_image_check.setChecked(False)
        self.invert_image_check.hide()
        self.threshold_enabled_check = QCheckBox("启用二值阈值", self)
        self.threshold_enabled_check.setObjectName("thresholdEnabled")
        self.threshold_enabled_check.setChecked(False)
        self.threshold_enabled_check.hide()
        self.raster_pwm_check = QCheckBox("灰度 PWM", self)
        self.raster_pwm_check.setObjectName("rasterPwmEnabled")
        self.raster_pwm_check.setChecked(True)
        self.raster_pwm_check.hide()
        self.auto_line_art_check = QCheckBox("自动识别黑白线稿并精简", self)
        self.auto_line_art_check.setObjectName("autoLineArtCompression")
        self.auto_line_art_check.setChecked(True)
        self.auto_line_art_check.hide()
        self.scan_direction_combo = QComboBox(self)
        self.scan_direction_combo.setObjectName("scanDirection")
        for label, data in SCAN_DIRECTION_LABELS:
            self.scan_direction_combo.addItem(label, data)
        self.scan_direction_combo.hide()
        self.unidirectional_scan_check = QCheckBox("单向扫描", self)
        self.unidirectional_scan_check.setObjectName("unidirectionalScan")
        self.unidirectional_scan_check.setChecked(False)
        self.unidirectional_scan_check.hide()
        self.rapid_white_check = QCheckBox("白区使用 G0 快速移动", self)
        self.rapid_white_check.setObjectName("rapidWhiteTravel")
        self.rapid_white_check.setChecked(True)
        self.rapid_white_check.hide()
        self.dither_algorithm_combo = QComboBox(self)
        self.dither_algorithm_combo.setObjectName("ditherAlgorithm")
        for label, data in DITHER_ALGORITHM_LABELS:
            self.dither_algorithm_combo.addItem(label, data)
        self.dither_algorithm_combo.hide()

        self.lasergrbl_brightness_spin = QSpinBox(self)
        self.lasergrbl_brightness_spin.setRange(0, 200)
        self.lasergrbl_brightness_spin.setValue(100)
        self.lasergrbl_brightness_spin.hide()
        self.lasergrbl_contrast_spin = QSpinBox(self)
        self.lasergrbl_contrast_spin.setRange(0, 200)
        self.lasergrbl_contrast_spin.setValue(100)
        self.lasergrbl_contrast_spin.hide()
        self.lasergrbl_white_clip_spin = QSpinBox(self)
        self.lasergrbl_white_clip_spin.setRange(0, 255)
        self.lasergrbl_white_clip_spin.setValue(5)
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
        self.vector_fill_combo.addItem("垂直填充", "vertical")
        self.vector_fill_combo.addItem("交叉填充", "cross")
        self.vector_fill_combo.addItem("45° 对角填充", "diagonal")
        self.vector_fill_combo.hide()
        self.fill_quality_spin = QDoubleSpinBox(self)
        self.fill_quality_spin.setObjectName("vectorFillQualityLinesPerMm")
        self.fill_quality_spin.setRange(1.0, MAX_SCAN_QUALITY_LINES_MM)
        self.fill_quality_spin.setDecimals(1)
        self.fill_quality_spin.setSingleStep(1.0)
        self.fill_quality_spin.setValue(DEFAULT_SCAN_QUALITY_LINES_MM)
        self.fill_quality_spin.hide()

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
        self.fill_speed_spin = QSpinBox(self)
        self.fill_speed_spin.setObjectName("fillSpeedMmMin")
        self.fill_speed_spin.setRange(100, 12_000)
        self.fill_speed_spin.setValue(3000)
        self.fill_speed_spin.hide()
        self.vector_quality_spin = QDoubleSpinBox(self)
        self.vector_quality_spin.setObjectName("vectorQualityPixelsPerMm")
        self.vector_quality_spin.setRange(2.0, 40.0)
        self.vector_quality_spin.setDecimals(1)
        self.vector_quality_spin.setSingleStep(1.0)
        self.vector_quality_spin.setValue(12.0)
        self.vector_quality_spin.hide()
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
    def _configure_dialog_number_inputs(
        dialog: QDialog,
    ) -> tuple[QAbstractSpinBox, ...]:
        """Keep typed numbers stable until the user finishes editing."""

        inputs = tuple(dialog.findChildren(QAbstractSpinBox))
        for spin in inputs:
            spin.setKeyboardTracking(False)
            spin.setAccelerated(True)
        return inputs

    @staticmethod
    def _commit_dialog_number_inputs(
        inputs: tuple[QAbstractSpinBox, ...],
    ) -> None:
        for spin in inputs:
            spin.interpretText()

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
            QSpinBox#dialogTargetDpi,
            QDoubleSpinBox#dialogMarkWidthMm,
            QDoubleSpinBox#dialogMarkHeightMm,
            QDoubleSpinBox#dialogOffsetX,
            QDoubleSpinBox#dialogOffsetY {
                padding-right:32px;
            }
            QSpinBox#dialogTargetDpi::up-button,
            QSpinBox#dialogTargetDpi::down-button,
            QDoubleSpinBox#dialogMarkWidthMm::up-button,
            QDoubleSpinBox#dialogMarkWidthMm::down-button,
            QDoubleSpinBox#dialogMarkHeightMm::up-button,
            QDoubleSpinBox#dialogMarkHeightMm::down-button,
            QDoubleSpinBox#dialogOffsetX::up-button,
            QDoubleSpinBox#dialogOffsetX::down-button,
            QDoubleSpinBox#dialogOffsetY::up-button,
            QDoubleSpinBox#dialogOffsetY::down-button {
                width:22px;
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
        left_panel.setMinimumWidth(300)
        left_layout = QVBoxLayout(left_panel)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.setSpacing(8)
        left_scroll = QScrollArea(dialog)
        left_scroll.setObjectName("laserDialogParameterScroll")
        left_scroll.setWidgetResizable(True)
        left_scroll.setFrameShape(QFrame.Shape.NoFrame)
        left_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        left_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        left_scroll.setFixedWidth(335)
        left_scroll.setWidget(left_panel)
        body.addWidget(left_scroll)

        right_panel = QWidget(dialog)
        right_layout = QVBoxLayout(right_panel)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(8)
        preview_tabs = QTabWidget(right_panel)
        preview_tabs.setObjectName("lasergrblPreviewTabs")
        preview_label = QLabel("未加载图像")
        preview_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        preview_label.setMinimumSize(420, 420)
        preview_label.setStyleSheet("background:#f1f5f9; color:#334155; border:1px solid #e2e8f0;")
        crop_source = self._source_image
        crop_enabled = not crop_source.isNull()
        if not crop_enabled:
            crop_source = QImage(1, 1, QImage.Format.Format_RGB32)
            crop_source.fill(QColor("white"))
        crop_widget = CropImageWidget(crop_source, right_panel)
        crop_widget.setObjectName("dialogCropImage")
        preview_tabs.addTab(preview_label, "裁切后效果")
        preview_tabs.addTab(crop_widget, "选择保留区域")
        right_layout.addWidget(preview_tabs, 1)

        transform_tip = QLabel("几何预处理")
        transform_tip.setObjectName("dialogGeometryTip")
        transform_tip.setStyleSheet("color:#64748b;")
        right_layout.addWidget(transform_tip)
        transform_actions = QWidget(right_panel)
        transform_actions.setObjectName("dialogGeometryActions")
        transform_tools = QHBoxLayout(transform_actions)
        transform_tools.setContentsMargins(0, 0, 0, 0)
        transform_tools.setSpacing(8)
        btn_rotate_left = QPushButton("左转 90°")
        btn_rotate_left.setObjectName("dialogRotateLeft")
        btn_rotate_right = QPushButton("右转 90°")
        btn_rotate_right.setObjectName("dialogRotateRight")
        btn_flip_horizontal = QPushButton("水平翻转")
        btn_flip_horizontal.setObjectName("dialogFlipHorizontal")
        btn_flip_vertical = QPushButton("垂直翻转")
        btn_flip_vertical.setObjectName("dialogFlipVertical")
        btn_invert_colors = QPushButton("反色")
        btn_invert_colors.setObjectName("dialogInvertColors")
        for button in (
            btn_rotate_left,
            btn_rotate_right,
            btn_flip_horizontal,
            btn_flip_vertical,
            btn_invert_colors,
        ):
            button.setEnabled(crop_enabled)
            button.setSizePolicy(
                QSizePolicy.Policy.Minimum,
                QSizePolicy.Policy.Fixed,
            )
            button.setMinimumWidth(
                button.fontMetrics().horizontalAdvance(button.text()) + 40
            )
            transform_tools.addWidget(button, 1)
        right_layout.addWidget(transform_actions)

        crop_tip = QLabel("在“选择保留区域”页拖动蓝框；蓝框内会保留")
        crop_tip.setObjectName("dialogCropTip")
        crop_tip.setStyleSheet("color:#64748b;")
        crop_tip.setWordWrap(True)
        right_layout.addWidget(crop_tip)
        crop_actions = QWidget(right_panel)
        crop_actions.setObjectName("dialogCropActions")
        crop_tools = QHBoxLayout(crop_actions)
        crop_tools.setContentsMargins(0, 0, 0, 0)
        crop_tools.setSpacing(8)
        crop_tools.addStretch(1)
        btn_auto_crop = QPushButton("自动裁剪主体")
        btn_auto_crop.setObjectName("dialogAutoCrop")
        btn_select_all = QPushButton("选择全部")
        btn_select_all.setObjectName("dialogSelectAll")
        btn_restore_original = QPushButton("恢复原图")
        btn_restore_original.setObjectName("dialogRestoreOriginal")
        for button in (btn_auto_crop, btn_select_all, btn_restore_original):
            button.setEnabled(crop_enabled)
            button.setSizePolicy(
                QSizePolicy.Policy.Minimum,
                QSizePolicy.Policy.Fixed,
            )
            button.setMinimumWidth(
                button.fontMetrics().horizontalAdvance(button.text()) + 40
            )
            crop_tools.addWidget(button)
        right_layout.addWidget(crop_actions)
        crop_status = QLabel("")
        crop_status.setObjectName("dialogCropStatus")
        crop_status.setWordWrap(True)
        crop_status.setMinimumHeight(50)
        right_layout.addWidget(crop_status)
        body.addWidget(right_panel, 1)
        using_original_image = [False]
        working_image_changed = [False]

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

        grayscale_formula = QComboBox(dialog)
        grayscale_formula.setObjectName("dialogGrayscaleFormula")
        for label, data in GRAYSCALE_FORMULA_LABELS:
            grayscale_formula.addItem(label, data)
        self._set_combo_data(
            grayscale_formula,
            self.grayscale_formula_combo.currentData(),
        )
        params_form.addRow("灰度公式", grayscale_formula)

        red_weight = QSpinBox(dialog)
        red_weight.setRange(0, 300)
        red_weight.setValue(self.grayscale_red_spin.value())
        red_weight.setPrefix("R ")
        green_weight = QSpinBox(dialog)
        green_weight.setRange(0, 300)
        green_weight.setValue(self.grayscale_green_spin.value())
        green_weight.setPrefix("G ")
        blue_weight = QSpinBox(dialog)
        blue_weight.setRange(0, 300)
        blue_weight.setValue(self.grayscale_blue_spin.value())
        blue_weight.setPrefix("B ")
        rgb_weight_row = QWidget(dialog)
        rgb_weight_layout = QHBoxLayout(rgb_weight_row)
        rgb_weight_layout.setContentsMargins(0, 0, 0, 0)
        rgb_weight_layout.setSpacing(5)
        rgb_weight_layout.addWidget(red_weight)
        rgb_weight_layout.addWidget(green_weight)
        rgb_weight_layout.addWidget(blue_weight)
        params_form.addRow("RGB 权重", rgb_weight_row)

        invert_check = QCheckBox("反色")
        invert_check.setObjectName("dialogInvertImage")
        invert_check.setChecked(self.invert_image_check.isChecked())
        threshold_toggle = QCheckBox("二值阈值")
        threshold_toggle.setObjectName("dialogThresholdEnabled")
        threshold_toggle.setChecked(self.threshold_enabled_check.isChecked())
        pwm_check = QCheckBox("灰度 PWM")
        pwm_check.setObjectName("dialogRasterPwmEnabled")
        pwm_check.setChecked(self.raster_pwm_check.isChecked())
        common_flags_row = QWidget(dialog)
        common_flags_layout = QHBoxLayout(common_flags_row)
        common_flags_layout.setContentsMargins(0, 0, 0, 0)
        common_flags_layout.setSpacing(8)
        common_flags_layout.addWidget(invert_check)
        common_flags_layout.addWidget(threshold_toggle)
        common_flags_layout.addWidget(pwm_check)
        params_form.addRow("处理", common_flags_row)
        auto_line_art_check = QCheckBox("自动识别黑白线稿并使用二值色段精简")
        auto_line_art_check.setObjectName("dialogAutoLineArtCompression")
        auto_line_art_check.setChecked(self.auto_line_art_check.isChecked())
        params_form.addRow("文件精简", auto_line_art_check)

        scan_direction = QComboBox(dialog)
        scan_direction.setObjectName("dialogScanDirection")
        for label, data in SCAN_DIRECTION_LABELS:
            scan_direction.addItem(label, data)
        self._set_combo_data(scan_direction, self.scan_direction_combo.currentData())
        params_form.addRow("扫描方向", scan_direction)

        unidirectional_check = QCheckBox("单向扫描（每行同方向）")
        unidirectional_check.setObjectName("dialogUnidirectionalScan")
        unidirectional_check.setChecked(self.unidirectional_scan_check.isChecked())
        params_form.addRow("", unidirectional_check)

        rapid_white_check = QCheckBox("白区使用 G0 快速移动")
        rapid_white_check.setObjectName("dialogRapidWhiteTravel")
        rapid_white_check.setChecked(self.rapid_white_check.isChecked())
        params_form.addRow("", rapid_white_check)

        dither_algorithm = QComboBox(dialog)
        dither_algorithm.setObjectName("dialogDitherAlgorithm")
        for label, data in DITHER_ALGORITHM_LABELS:
            dither_algorithm.addItem(label, data)
        self._set_combo_data(
            dither_algorithm,
            self.dither_algorithm_combo.currentData(),
        )
        params_form.addRow("抖动算法", dither_algorithm)

        scan_quality = QDoubleSpinBox(dialog)
        scan_quality.setObjectName("dialogScanQualityLinesPerMm")
        scan_quality.setRange(1.0, MAX_SCAN_QUALITY_LINES_MM)
        scan_quality.setDecimals(1)
        scan_quality.setSingleStep(1.0)
        scan_quality.setValue(self.scan_quality_spin.value())
        scan_quality.setSuffix(" 线/mm")
        quality_hint = QLabel("")
        quality_hint.setStyleSheet("color:#64748b;")
        quality_row = QWidget(dialog)
        quality_layout = QHBoxLayout(quality_row)
        quality_layout.setContentsMargins(0, 0, 0, 0)
        quality_layout.setSpacing(8)
        quality_layout.addWidget(scan_quality)
        quality_layout.addWidget(quality_hint, 1)
        params_form.addRow("扫描质量", quality_row)

        row, threshold = int_slider(self.threshold_spin.value(), 0, 255)
        threshold.setObjectName("dialogThresholdValue")
        params_form.addRow("阈值", row)
        row, brightness = int_slider(self.lasergrbl_brightness_spin.value(), 0, 200)
        brightness.setObjectName("dialogBrightnessValue")
        params_form.addRow("亮度", row)
        row, contrast = int_slider(self.lasergrbl_contrast_spin.value(), 0, 200)
        contrast.setObjectName("dialogContrastValue")
        params_form.addRow("对比度", row)
        row, white_clip = int_slider(self.lasergrbl_white_clip_spin.value(), 0, 255)
        white_clip.setObjectName("dialogWhiteClipValue")
        params_form.addRow("白色限制", row)
        row, black_clip = int_slider(self.lasergrbl_black_clip_spin.value(), 0, 255)
        black_clip.setObjectName("dialogBlackClipValue")
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

        vector_quality = QDoubleSpinBox(dialog)
        vector_quality.setObjectName("dialogVectorQualityPixelsPerMm")
        vector_quality.setRange(2.0, 40.0)
        vector_quality.setDecimals(1)
        vector_quality.setSingleStep(1.0)
        vector_quality.setValue(self.vector_quality_spin.value())
        vector_quality.setSuffix(" px/mm")
        vector_form.addRow("矢量输入质量", vector_quality)

        fill_combo = QComboBox(dialog)
        fill_combo.addItem("无填充", "none")
        fill_combo.addItem("扫描填充", "scanline")
        fill_combo.addItem("垂直填充", "vertical")
        fill_combo.addItem("交叉填充", "cross")
        fill_combo.addItem("45° 对角填充", "diagonal")
        self._set_combo_data(fill_combo, self.vector_fill_combo.currentData())
        vector_form.addRow("填充", fill_combo)

        fill_quality = QDoubleSpinBox(dialog)
        fill_quality.setObjectName("dialogVectorFillQualityLinesPerMm")
        fill_quality.setRange(1.0, MAX_SCAN_QUALITY_LINES_MM)
        fill_quality.setDecimals(1)
        fill_quality.setSingleStep(1.0)
        fill_quality.setValue(self.fill_quality_spin.value())
        fill_quality.setSuffix(" 线/mm")
        vector_form.addRow("填充质量", fill_quality)

        def update_dialog_vector_controls() -> None:
            mode = tool_combo.currentData()
            enabled = mode in ("vector", "lasergrbl_vector", "centerline")
            vector_enabled = mode in ("vector", "lasergrbl_vector")
            fill_enabled = mode in ("vector", "lasergrbl_vector")
            downsample_enabled = mode in ("vector", "lasergrbl_vector")
            scan_enabled = mode in RASTER_EXTRACTION_MODES
            vector_group.setVisible(enabled)
            spot_check.setEnabled(enabled)
            spot_area.setEnabled(enabled)
            smooth_check.setEnabled(vector_enabled)
            smooth_value.setEnabled(vector_enabled)
            optimize_check.setEnabled(vector_enabled)
            adaptive_check.setEnabled(enabled)
            downsample_check.setEnabled(downsample_enabled)
            downsample_value.setEnabled(downsample_enabled)
            vector_quality.setEnabled(vector_enabled)
            fill_combo.setEnabled(fill_enabled)
            fill_quality.setEnabled(
                fill_enabled and fill_combo.currentData() != "none"
            )
            scan_quality.setEnabled(scan_enabled)
            quality_hint.setEnabled(scan_enabled)
            scan_direction.setEnabled(mode in RASTER_EXTRACTION_MODES)
            unidirectional_check.setEnabled(mode in RASTER_EXTRACTION_MODES)
            rapid_white_check.setEnabled(
                mode in RASTER_EXTRACTION_MODES
                or (fill_enabled and fill_combo.currentData() != "none")
            )
            dither_algorithm.setEnabled(mode == "dither")
            pwm_check.setEnabled(mode in ("scanline", "passthrough"))
            auto_line_art_check.setEnabled(mode in ("scanline", "passthrough"))
            threshold_toggle.setEnabled(mode in ("scanline", "passthrough"))
            threshold.setEnabled(
                mode in ("dither", "vector", "lasergrbl_vector", "centerline")
                or threshold_toggle.isChecked()
                or not pwm_check.isChecked()
            )
            custom = grayscale_formula.currentData() == "custom"
            for weight in (red_weight, green_weight, blue_weight):
                weight.setEnabled(custom)

        tool_combo.currentIndexChanged.connect(lambda *_: update_dialog_vector_controls())
        fill_combo.currentIndexChanged.connect(lambda *_: update_dialog_vector_controls())
        grayscale_formula.currentIndexChanged.connect(
            lambda *_: update_dialog_vector_controls()
        )
        threshold_toggle.toggled.connect(lambda *_: update_dialog_vector_controls())
        pwm_check.toggled.connect(lambda *_: update_dialog_vector_controls())
        update_dialog_vector_controls()

        left_layout.addStretch(1)

        def make_preview_job() -> Callable[[], QImage]:
            selected_source = crop_widget.selected_image()
            transform_mode = (
                Qt.FastTransformation
                if resize_combo.currentData() == "nearest"
                else Qt.SmoothTransformation
            )
            mode = str(tool_combo.currentData())
            longest = max(1, selected_source.width(), selected_source.height())
            preview_factor = min(1.0, DIALOG_PREVIEW_MAX_PX / longest)
            width_px = max(1, round(selected_source.width() * preview_factor))
            height_px = max(1, round(selected_source.height() * preview_factor))
            formula_value = str(grayscale_formula.currentData())
            red_value = red_weight.value()
            green_value = green_weight.value()
            blue_value = blue_weight.value()
            invert_value = invert_check.isChecked()
            threshold_value = threshold.value()
            brightness_value = brightness.value()
            contrast_value = contrast.value()
            white_clip_value = white_clip.value()
            black_clip_value = black_clip.value()
            min_area = float(spot_area.value()) if spot_check.isChecked() else 1.0
            smoothing = smooth_value.value() if smooth_check.isChecked() else 0.0
            adaptive = adaptive_check.isChecked()
            optimize = optimize_check.isChecked()
            downsample = (
                downsample_check.isChecked()
                and mode in ("vector", "lasergrbl_vector")
                and downsample_value.value() > 1.0
            )
            downsample_factor = downsample_value.value()
            dither_mode = str(dither_algorithm.currentData())
            fill_mode = str(fill_combo.currentData())
            threshold_enabled = threshold_toggle.isChecked()
            pwm_enabled = pwm_check.isChecked()
            auto_line_art = auto_line_art_check.isChecked()
            power_min = self.power_min_spin.value()
            power_max = self.power_spin.value()

            def compute() -> QImage:
                image = selected_source.scaled(
                    width_px,
                    height_px,
                    Qt.AspectRatioMode.IgnoreAspectRatio,
                    transform_mode,
                )
                if downsample:
                    image = image.scaled(
                        max(1, int(image.width() / downsample_factor)),
                        max(1, int(image.height() / downsample_factor)),
                        Qt.KeepAspectRatio,
                        Qt.FastTransformation,
                    )
                rows = self._grayscale_rows_from_image(
                    image,
                    formula=formula_value,
                    red_weight=red_value,
                    green_weight=green_value,
                    blue_weight=blue_value,
                    invert=invert_value,
                )
                tone_rows = apply_lasergrbl_tone(
                    rows,
                    brightness=brightness_value,
                    contrast=contrast_value,
                    white_clip=white_clip_value,
                    black_clip=black_clip_value,
                )
                if mode == "dither":
                    return self._build_mask_preview(
                        dither_rows_to_mask(
                            tone_rows,
                            threshold_value,
                            dither_mode,
                        )
                    )
                if mode == "centerline":
                    base = prepare_laser_mask(
                        tone_rows,
                        threshold_value,
                        adaptive=adaptive,
                        min_area_px=min_area,
                    )
                    return self._build_mask_preview(centerline_mask(base))
                if mode in ("vector", "lasergrbl_vector"):
                    if mode == "lasergrbl_vector":
                        result = lasergrbl_style_vectorize(
                            rows,
                            LaserGrblVectorOptions(
                                threshold=threshold_value,
                                brightness=brightness_value,
                                contrast=contrast_value,
                                white_clip=white_clip_value,
                                black_clip=black_clip_value,
                                spot_remove_px=min_area,
                                smoothing_px=smoothing,
                                optimize_paths=optimize,
                                curve_smoothing=smoothing,
                                adaptive_quality=adaptive,
                            ),
                        )
                        contour_rows = result.rows
                        contours = result.contours
                    else:
                        mask = prepare_laser_mask(
                            tone_rows,
                            threshold_value,
                            adaptive=True,
                            edge_enhance=True,
                            min_area_px=min_area,
                        )
                        contours = simplify_vector_contours(
                            trace_vector_contours_from_mask(
                                mask,
                                min_area_px=min_area,
                            ),
                            tolerance_px=smoothing,
                        )
                        contour_rows = tone_rows
                    if fill_mode == "none":
                        return self._build_vector_preview(contour_rows, contours)
                    fill_mask = prepare_laser_mask(
                        tone_rows,
                        threshold_value,
                        adaptive=mode != "lasergrbl_vector",
                        edge_enhance=mode == "vector",
                        min_area_px=min_area,
                    )
                    return self._build_vector_fill_preview(
                        fill_mask,
                        contours,
                        image.width(),
                        image.height(),
                    )

                use_binary = (
                    mode == "passthrough" and threshold_enabled
                    or mode == "scanline"
                    and (threshold_enabled or not pwm_enabled)
                    or mode in ("scanline", "passthrough")
                    and auto_line_art
                    and self._looks_like_binary_line_art(tone_rows)
                )
                if use_binary:
                    return self._build_mask_preview(
                        prepare_laser_mask(
                            tone_rows,
                            threshold_value,
                            adaptive=False,
                            min_area_px=0,
                        )
                    )
                return self._build_grayscale_tone_preview(
                    tone_rows,
                    power_min_s=power_min,
                    power_max_s=power_max,
                )

            return compute

        preview_timer = QTimer(dialog)
        preview_timer.setObjectName("dialogPreviewDebounce")
        preview_timer.setSingleShot(True)
        preview_timer.setInterval(DIALOG_PREVIEW_DEBOUNCE_MS)
        preview_serial = [0]
        dialog_alive = [True]
        preview_tasks: set[_PreviewTask] = set()

        def preview_ready(
            serial: int,
            preview: QImage,
            task: _PreviewTask,
        ) -> None:
            preview_tasks.discard(task)
            if not dialog_alive[0] or serial != preview_serial[0]:
                return
            preview_label.setPixmap(
                QPixmap.fromImage(preview).scaled(
                    preview_label.size(),
                    Qt.KeepAspectRatio,
                    Qt.SmoothTransformation,
                )
            )

        def preview_failed(serial: int, message: str, task: _PreviewTask) -> None:
            preview_tasks.discard(task)
            if not dialog_alive[0] or serial != preview_serial[0]:
                return
            preview_label.setText(f"预览生成失败\n{message}")

        def start_preview() -> None:
            if not crop_enabled or not dialog_alive[0]:
                return
            serial = preview_serial[0]
            task = _PreviewTask(serial, make_preview_job())
            preview_tasks.add(task)
            task.signals.ready.connect(
                lambda result_serial, image, current=task: preview_ready(
                    result_serial,
                    image,
                    current,
                )
            )
            task.signals.failed.connect(
                lambda result_serial, message, current=task: preview_failed(
                    result_serial,
                    message,
                    current,
                )
            )
            self._image_preview_pool.start(task)

        def schedule_preview(*_args, immediate: bool = False) -> None:
            if not crop_enabled:
                return
            preview_serial[0] += 1
            self._image_preview_pool.clear()
            preview_timer.start(0 if immediate else DIALOG_PREVIEW_DEBOUNCE_MS)

        preview_timer.timeout.connect(start_preview)

        def update_crop_status(rect: QRect) -> None:
            image_rect = crop_widget.image_rect()
            selection_is_full_image = rect == image_rect
            current_width = max(1, image_rect.width())
            current_height = max(1, image_rect.height())
            if selection_is_full_image:
                original_size = self._original_image.size()
                current_was_preprocessed = (
                    not self._original_image.isNull()
                    and (
                        original_size.width() != current_width
                        or original_size.height() != current_height
                    )
                )
                if current_was_preprocessed:
                    crop_status.setText(
                        "✓ 当前已使用处理后的图像："
                        f"原图 {original_size.width()}×{original_size.height()} px → "
                        f"当前 {current_width}×{current_height} px。"
                        "没有新的待应用裁切。"
                    )
                    crop_status.setStyleSheet(
                        "QLabel#dialogCropStatus { color:#047857; background:#ecfdf5; "
                        "border:1px solid #a7f3d0; border-radius:6px; padding:7px 10px; "
                        "font-weight:600; }"
                    )
                else:
                    crop_status.setText(
                        f"当前保留整张图：{current_width}×{current_height} px。"
                        "拖动蓝框或点击“自动裁剪主体”后，这里会显示裁切前后对比。"
                    )
                    crop_status.setStyleSheet(
                        "QLabel#dialogCropStatus { color:#475569; background:#f8fafc; "
                        "border:1px solid #cbd5e1; border-radius:6px; padding:7px 10px; }"
                    )
                preview_tabs.setTabText(0, "处理后预览")
            else:
                total_area = max(1, current_width * current_height)
                kept_area = max(1, rect.width() * rect.height())
                removed_percent = max(
                    0.0,
                    min(100.0, (1.0 - kept_area / total_area) * 100.0),
                )
                crop_status.setText(
                    "待应用裁切："
                    f"{current_width}×{current_height} px → "
                    f"{rect.width()}×{rect.height()} px，"
                    f"将去除 {removed_percent:.1f}% 画布。\n"
                    "蓝框内为最终保留内容；点击右下角“应用参数与裁切”后生效。"
                )
                crop_status.setStyleSheet(
                    "QLabel#dialogCropStatus { color:#9a3412; background:#fff7ed; "
                    "border:1px solid #fdba74; border-radius:6px; padding:7px 10px; "
                    "font-weight:600; }"
                )
                preview_tabs.setTabText(0, "裁切后效果（待应用）")
            source_width = max(1, rect.width())
            source_height = max(1, rect.height())
            target_width = float(self.size_spin.value())
            target_height = float(self.target_height_spin.value())
            mark_scale = min(
                target_width / source_width,
                target_height / source_height,
            )
            actual_width = source_width * mark_scale
            actual_height = source_height * mark_scale
            width_px = max(1, round(actual_width * scan_quality.value()))
            height_px = max(1, round(actual_height * scan_quality.value()))
            selected_direction = str(scan_direction.currentData())
            line_count = {
                "vertical": width_px,
                "diagonal": width_px + height_px - 1,
            }.get(selected_direction, height_px)
            quality_hint.setText(
                f"预计 {line_count} 条轨迹"
            )

        for widget in (
            resize_combo,
            grayscale_formula,
            red_weight,
            green_weight,
            blue_weight,
            invert_check,
            threshold_toggle,
            pwm_check,
            auto_line_art_check,
            dither_algorithm,
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
                widget.currentIndexChanged.connect(schedule_preview)
            elif isinstance(widget, (QSpinBox, QDoubleSpinBox)):
                widget.valueChanged.connect(schedule_preview)
            elif isinstance(widget, QCheckBox):
                widget.toggled.connect(schedule_preview)

        def crop_selection_changed(rect: QRect) -> None:
            update_crop_status(rect)

        def auto_select_subject() -> None:
            btn_auto_crop.setEnabled(False)
            crop_status.setText("正在识别主体边界...")
            QApplication.processEvents()
            try:
                result = auto_crop_white_background(crop_widget.current_image())
            except Exception as exc:
                crop_status.setText(f"自动裁剪失败：{exc}")
                btn_auto_crop.setEnabled(True)
                return
            crop_widget.set_selection_rect(result.crop_rect)
            btn_auto_crop.setEnabled(True)
            if not result.cropped:
                crop_status.setText(
                    "未检测到可去除的均匀背景，已保留整张图。"
                    "如果仍需裁切，可直接拖动鼠标选择保留区域。"
                )
            preview_tabs.setCurrentWidget(crop_widget)

        def transform_dialog_image(operation: str) -> None:
            image = crop_widget.selected_image()
            if operation == "rotate_left":
                image = rotate_image_quarter_turns(image, 3)
            elif operation == "rotate_right":
                image = rotate_image_quarter_turns(image, 1)
            elif operation == "flip_horizontal":
                image = flip_image(image, horizontal=True)
            elif operation == "flip_vertical":
                image = flip_image(image, vertical=True)
            else:
                return
            crop_widget.set_image(image)
            working_image_changed[0] = True
            using_original_image[0] = False
            preview_tabs.setCurrentWidget(crop_widget)

        def restore_dialog_original() -> None:
            if self._original_image.isNull():
                return
            using_original_image[0] = True
            working_image_changed[0] = True
            crop_widget.set_image(self._original_image)
            preview_tabs.setCurrentWidget(crop_widget)

        crop_widget.selection_changed.connect(crop_selection_changed)
        crop_widget.selection_committed.connect(schedule_preview)
        btn_auto_crop.clicked.connect(auto_select_subject)
        btn_select_all.clicked.connect(crop_widget.reset_selection)
        btn_restore_original.clicked.connect(restore_dialog_original)
        btn_rotate_left.clicked.connect(
            lambda: transform_dialog_image("rotate_left")
        )
        btn_rotate_right.clicked.connect(
            lambda: transform_dialog_image("rotate_right")
        )
        btn_flip_horizontal.clicked.connect(
            lambda: transform_dialog_image("flip_horizontal")
        )
        btn_flip_vertical.clicked.connect(
            lambda: transform_dialog_image("flip_vertical")
        )
        btn_invert_colors.clicked.connect(invert_check.toggle)
        scan_quality.valueChanged.connect(lambda *_: update_crop_status(crop_widget.selection_rect()))
        scan_direction.currentIndexChanged.connect(
            lambda *_: update_crop_status(crop_widget.selection_rect())
        )
        update_crop_status(crop_widget.selection_rect())
        schedule_preview(immediate=True)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("应用参数与裁切")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("取消")
        layout.addWidget(buttons)
        numeric_inputs = self._configure_dialog_number_inputs(dialog)

        def accept() -> None:
            self._commit_dialog_number_inputs(numeric_inputs)
            self._set_combo_data(self.resize_mode_combo, resize_combo.currentData())
            self.extract_mode.setCurrentIndex(self.extract_mode.findData(tool_combo.currentData()))
            self.threshold_spin.setValue(threshold.value())
            self.scan_quality_spin.setValue(scan_quality.value())
            self._set_combo_data(
                self.grayscale_formula_combo,
                grayscale_formula.currentData(),
            )
            self.grayscale_red_spin.setValue(red_weight.value())
            self.grayscale_green_spin.setValue(green_weight.value())
            self.grayscale_blue_spin.setValue(blue_weight.value())
            self.invert_image_check.setChecked(invert_check.isChecked())
            self.threshold_enabled_check.setChecked(threshold_toggle.isChecked())
            self.raster_pwm_check.setChecked(pwm_check.isChecked())
            self.auto_line_art_check.setChecked(auto_line_art_check.isChecked())
            self._set_combo_data(
                self.scan_direction_combo,
                scan_direction.currentData(),
            )
            self.unidirectional_scan_check.setChecked(
                unidirectional_check.isChecked()
            )
            self.rapid_white_check.setChecked(rapid_white_check.isChecked())
            self._set_combo_data(
                self.dither_algorithm_combo,
                dither_algorithm.currentData(),
            )
            self.lasergrbl_brightness_spin.setValue(brightness.value())
            self.lasergrbl_contrast_spin.setValue(contrast.value())
            safe_white_clip = white_clip.value()
            safe_black_clip = min(
                black_clip.value(),
                max(0, 254 - safe_white_clip),
            )
            self.lasergrbl_white_clip_spin.setValue(safe_white_clip)
            self.lasergrbl_black_clip_spin.setValue(safe_black_clip)
            self.spot_removal_check.setChecked(spot_check.isChecked())
            self.vector_min_area.setValue(spot_area.value())
            self.smoothing_check.setChecked(smooth_check.isChecked())
            self.vector_simplify.setValue(smooth_value.value())
            self.optimize_paths_check.setChecked(optimize_check.isChecked())
            self.adaptive_quality_check.setChecked(adaptive_check.isChecked())
            self.downsample_check.setChecked(downsample_check.isChecked())
            self.downsample_factor.setValue(downsample_value.value())
            self.vector_quality_spin.setValue(vector_quality.value())
            self._set_combo_data(self.vector_fill_combo, fill_combo.currentData())
            self.fill_quality_spin.setValue(fill_quality.value())
            if crop_enabled:
                if working_image_changed[0]:
                    previous_size = self._source_image.size()
                    selection_is_crop = (
                        crop_widget.selection_rect() != crop_widget.image_rect()
                    )
                    self._source_image = crop_widget.selected_image()
                    self._converted_image = QImage()
                    if selection_is_crop:
                        action = "✓ 裁剪已生效"
                        self.btn_preview_original.setText("裁切后")
                    elif using_original_image[0]:
                        action = "已恢复原图"
                        self.btn_preview_original.setText("原图")
                    else:
                        action = "✓ 几何处理已生效"
                        self.btn_preview_original.setText("处理后")
                    self._source_status = (
                        f"{action} · "
                        f"{Path(self._image_path).name if self._image_path else '生成图像'} · "
                        f"{previous_size.width()}×{previous_size.height()} → "
                        f"{self._source_image.width()}×{self._source_image.height()} px"
                    )
                    self._converted_status = ""
                    self.btn_preview_converted.setEnabled(False)
                    self._refresh_target_size_for_source()
                    self._set_preview_mode("original")
                elif crop_widget.selection_rect() != crop_widget.image_rect():
                    self._apply_crop(crop_widget.selection_rect())
            dialog.accept()

        buttons.accepted.connect(accept)
        buttons.rejected.connect(dialog.reject)
        dialog.exec()
        dialog_alive[0] = False
        preview_serial[0] += 1
        preview_timer.stop()
        self._image_preview_pool.clear()

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
        size_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow
        )
        size_form.setHorizontalSpacing(16)
        size_form.setVerticalSpacing(8)
        body_layout.addWidget(size_group)

        laser_mode = QComboBox(dialog)
        laser_mode.addItem("M4 - Dynamic Power", "M4")
        laser_mode.addItem("M3 - Constant Power", "M3")
        self._set_combo_data(laser_mode, self.laser_mode_combo.currentData())
        laser_form.addRow("激光模式", laser_mode)

        speed = QSpinBox(dialog)
        speed.setObjectName("dialogBoundarySpeedMmMin")
        speed.setRange(100, 12_000)
        speed.setSingleStep(100)
        speed.setValue(self.speed_spin.value())
        speed.setFixedWidth(140)
        intensity_form.addRow("边界速度 mm/min", speed)
        fill_speed = QSpinBox(dialog)
        fill_speed.setObjectName("dialogFillSpeedMmMin")
        fill_speed.setRange(100, 12_000)
        fill_speed.setSingleStep(100)
        fill_speed.setValue(self.fill_speed_spin.value())
        fill_speed.setFixedWidth(140)
        intensity_form.addRow("扫描/填充速度 mm/min", fill_speed)
        min_s = QSpinBox(dialog)
        min_s.setObjectName("dialogMinimumPowerS")
        min_s.setRange(0, 1000)
        min_s.setSingleStep(10)
        min_s.setValue(self.power_min_spin.value())
        min_s.setFixedWidth(140)
        laser_form.addRow("最小 S 值", min_s)
        max_s = QSpinBox(dialog)
        max_s.setObjectName("dialogMaximumPowerS")
        max_s.setRange(0, 1000)
        max_s.setSingleStep(10)
        max_s.setValue(self.power_spin.value())
        max_s.setFixedWidth(140)
        laser_form.addRow("最大 S 值", max_s)

        auto_size = QCheckBox("自动调整大小")
        auto_size.setObjectName("dialogAutoTargetSize")
        auto_size.setChecked(self.auto_size_check.isChecked())
        dpi = QSpinBox(dialog)
        dpi.setObjectName("dialogTargetDpi")
        dpi.setRange(50, 1200)
        dpi.setSingleStep(10)
        dpi.setValue(self.dpi_spin.value())
        dpi.setMinimumWidth(130)
        dpi.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        dpi.setPrefix("DPI: ")
        auto_row = QWidget(size_group)
        auto_row.setObjectName("dialogAutoSizeRow")
        auto_row.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Preferred,
        )
        auto_layout = QHBoxLayout(auto_row)
        auto_layout.setContentsMargins(0, 0, 0, 0)
        auto_layout.setSpacing(12)
        auto_layout.addWidget(dpi, 1)
        auto_layout.addWidget(auto_size)
        size_form.addRow("自动大小", auto_row)

        width = QDoubleSpinBox(dialog)
        width.setObjectName("dialogMarkWidthMm")
        width.setRange(0.0, 99.0)
        width.setDecimals(1)
        width.setSingleStep(0.1)
        width.setValue(self.size_spin.value())
        width.setMinimumWidth(130)
        width.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        width.setPrefix("W: ")
        height = QDoubleSpinBox(dialog)
        height.setObjectName("dialogMarkHeightMm")
        height.setRange(0.0, 99.0)
        height.setDecimals(1)
        height.setSingleStep(0.1)
        height.setValue(self.target_height_spin.value())
        height.setMinimumWidth(130)
        height.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        height.setPrefix("H: ")
        lock = QCheckBox("锁定")
        lock.setObjectName("dialogLockAspect")
        lock.setChecked(self.lock_aspect_check.isChecked())
        size_row = QWidget(size_group)
        size_row.setObjectName("dialogMarkSizeRow")
        size_row.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Preferred,
        )
        size_layout = QHBoxLayout(size_row)
        size_layout.setContentsMargins(0, 0, 0, 0)
        size_layout.setSpacing(12)
        size_layout.addWidget(width, 1)
        size_layout.addWidget(height, 1)
        size_layout.addWidget(lock)
        size_form.addRow("大小", size_row)

        source_ratio = self._source_aspect_ratio()
        ratio_hint = QLabel(size_group)
        ratio_hint.setObjectName("dialogSourceAspectRatio")
        if source_ratio is None:
            ratio_hint.setText("尚未加载图像，无法计算宽高比")
        else:
            ratio_hint.setText(
                f"裁切后图像：{self._source_image.width()}×{self._source_image.height()} px · "
                f"宽高比 {source_ratio:.4g}:1"
            )
        ratio_hint.setStyleSheet("color:#64748b; font-size:11px;")
        ratio_hint.setWordWrap(True)
        size_form.addRow("比例", ratio_hint)

        offset_x = QDoubleSpinBox(dialog)
        offset_x.setObjectName("dialogOffsetX")
        offset_x.setRange(0.0, 99.0)
        offset_x.setDecimals(1)
        offset_x.setSingleStep(0.1)
        offset_x.setValue(self.offset_x_spin.value())
        offset_x.setMinimumWidth(130)
        offset_x.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        offset_x.setPrefix("X: ")
        offset_y = QDoubleSpinBox(dialog)
        offset_y.setObjectName("dialogOffsetY")
        offset_y.setRange(0.0, 99.0)
        offset_y.setDecimals(1)
        offset_y.setSingleStep(0.1)
        offset_y.setValue(self.offset_y_spin.value())
        offset_y.setMinimumWidth(130)
        offset_y.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        offset_y.setPrefix("Y: ")
        offset_row = QWidget(size_group)
        offset_row.setObjectName("dialogOffsetRow")
        offset_row.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Preferred,
        )
        offset_layout = QHBoxLayout(offset_row)
        offset_layout.setContentsMargins(0, 0, 0, 0)
        offset_layout.setSpacing(12)
        offset_layout.addWidget(offset_x, 1)
        offset_layout.addWidget(offset_y, 1)
        size_form.addRow("偏移", offset_row)

        updating_size = [False]

        def set_dialog_size(width_mm: float, height_mm: float) -> None:
            updating_size[0] = True
            width.blockSignals(True)
            height.blockSignals(True)
            width.setValue(max(0.0, min(99.0, float(width_mm))))
            height.setValue(max(0.0, min(99.0, float(height_mm))))
            width.blockSignals(False)
            height.blockSignals(False)
            updating_size[0] = False

        def available_workspace() -> tuple[float, float]:
            return (
                max(0.0, 99.0 - float(offset_x.value())),
                max(0.0, 99.0 - float(offset_y.value())),
            )

        def apply_dialog_auto_size() -> None:
            if not auto_size.isChecked() or source_ratio is None:
                return
            auto_width = self._source_image.width() / max(1, dpi.value()) * 25.4
            auto_height = self._source_image.height() / max(1, dpi.value()) * 25.4
            available_width, available_height = available_workspace()
            fitted_width, fitted_height = self._fit_size_inside(
                auto_width,
                auto_height,
                available_width,
                available_height,
            )
            set_dialog_size(fitted_width, fitted_height)

        def sync_dialog_from_width(value: float) -> None:
            if (
                updating_size[0]
                or auto_size.isChecked()
                or not lock.isChecked()
                or source_ratio is None
            ):
                return
            fitted_width, fitted_height = self._aspect_size_from_width(
                float(value),
                source_ratio,
                *available_workspace(),
            )
            set_dialog_size(fitted_width, fitted_height)

        def sync_dialog_from_height(value: float) -> None:
            if (
                updating_size[0]
                or auto_size.isChecked()
                or not lock.isChecked()
                or source_ratio is None
            ):
                return
            fitted_width, fitted_height = self._aspect_size_from_height(
                float(value),
                source_ratio,
                *available_workspace(),
            )
            set_dialog_size(fitted_width, fitted_height)

        def update_size_controls() -> None:
            automatic = auto_size.isChecked() and source_ratio is not None
            dpi.setEnabled(automatic)
            width.setEnabled(not automatic)
            height.setEnabled(not automatic)
            lock.setEnabled(not automatic and source_ratio is not None)
            if automatic:
                apply_dialog_auto_size()
            elif lock.isChecked() and source_ratio is not None:
                sync_dialog_from_width(width.value())

        width.valueChanged.connect(sync_dialog_from_width)
        height.valueChanged.connect(sync_dialog_from_height)
        lock.toggled.connect(lambda *_: update_size_controls())
        auto_size.toggled.connect(lambda *_: update_size_controls())
        dpi.valueChanged.connect(lambda *_: apply_dialog_auto_size())
        offset_x.valueChanged.connect(
            lambda *_: apply_dialog_auto_size()
            if auto_size.isChecked()
            else sync_dialog_from_width(width.value())
        )
        offset_y.valueChanged.connect(
            lambda *_: apply_dialog_auto_size()
            if auto_size.isChecked()
            else sync_dialog_from_width(width.value())
        )
        update_size_controls()
        body_layout.addStretch(1)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        layout.addWidget(buttons)
        numeric_inputs = self._configure_dialog_number_inputs(dialog)

        def accept() -> None:
            self._commit_dialog_number_inputs(numeric_inputs)
            self._set_combo_data(self.laser_mode_combo, laser_mode.currentData())
            self.speed_spin.setValue(speed.value())
            self.fill_speed_spin.setValue(fill_speed.value())
            self.power_min_spin.setValue(min(min_s.value(), max_s.value()))
            self.power_spin.setValue(max(min_s.value(), max_s.value()))
            target_widgets = (
                self.auto_size_check,
                self.dpi_spin,
                self.size_spin,
                self.target_height_spin,
                self.lock_aspect_check,
                self.offset_x_spin,
                self.offset_y_spin,
            )
            for widget in target_widgets:
                widget.blockSignals(True)
            self.auto_size_check.setChecked(auto_size.isChecked())
            self.dpi_spin.setValue(dpi.value())
            self.lock_aspect_check.setChecked(lock.isChecked())
            self.offset_x_spin.setValue(offset_x.value())
            self.offset_y_spin.setValue(offset_y.value())
            self.size_spin.setValue(width.value())
            self.target_height_spin.setValue(height.value())
            for widget in target_widgets:
                widget.blockSignals(False)
            self._refresh_target_size_for_source()
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
        self.btn_adjust_gcode = QPushButton("调整参数…")
        self.btn_adjust_gcode.setObjectName("btnGcodeAdjust")
        self.btn_adjust_gcode.setToolTip(
            "调整已打开 G-code 的功率、速度、等比缩放和整体偏移"
        )
        self.btn_adjust_gcode.setEnabled(False)
        self.btn_adjust_gcode.clicked.connect(self._handle_gcode_adjust_action)
        toolbar.addWidget(self.btn_adjust_gcode)
        self.btn_load_full_gcode = QPushButton("加载完整内容")
        self.btn_load_full_gcode.setToolTip(
            "大文件默认只显示开头预览；点击后将完整内容载入编辑器"
        )
        self.btn_load_full_gcode.clicked.connect(self._load_full_gcode_into_editor)
        self.btn_load_full_gcode.hide()
        toolbar.addWidget(self.btn_load_full_gcode)
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
        self.editor = QPlainTextEdit()
        self.editor.setPlaceholderText("在此粘贴 G-code 或点击打开文件...")
        editor_font = self.editor.font()
        editor_font.setFamily("Consolas")
        self.editor.setFont(editor_font)
        self.editor.setStyleSheet("border:none; border-radius:0; background:#ffffff;")
        self.editor.textChanged.connect(self._update_size)
        self.editor.textChanged.connect(self._update_gcode_adjust_action)
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
            self.adaptive_quality_check,
        ):
            widget.setEnabled(enabled)
        self.downsample_check.setEnabled(mode in ("vector", "lasergrbl_vector"))
        self.downsample_factor.setEnabled(mode in ("vector", "lasergrbl_vector"))
        self.vector_quality_spin.setEnabled(mode in ("vector", "lasergrbl_vector"))
        self.vector_fill_combo.setEnabled(fill_enabled)
        self.fill_quality_spin.setEnabled(
            fill_enabled and self.vector_fill_combo.currentData() != "none"
        )

    def _source_aspect_ratio(self) -> float | None:
        if self._source_image.isNull() or self._source_image.height() <= 0:
            return None
        return self._source_image.width() / self._source_image.height()

    @staticmethod
    def _fit_size_inside(
        width_mm: float,
        height_mm: float,
        max_width_mm: float = 99.0,
        max_height_mm: float = 99.0,
    ) -> tuple[float, float]:
        width_value = max(0.0, float(width_mm))
        height_value = max(0.0, float(height_mm))
        max_width = max(0.0, float(max_width_mm))
        max_height = max(0.0, float(max_height_mm))
        if (
            width_value <= 0.0
            or height_value <= 0.0
            or max_width <= 0.0
            or max_height <= 0.0
        ):
            return 0.0, 0.0
        scale = min(
            1.0,
            max_width / width_value,
            max_height / height_value,
        )
        return width_value * scale, height_value * scale

    @classmethod
    def _aspect_size_from_width(
        cls,
        width_mm: float,
        aspect_ratio: float,
        max_width_mm: float = 99.0,
        max_height_mm: float = 99.0,
    ) -> tuple[float, float]:
        ratio = max(1e-9, float(aspect_ratio))
        width_value = max(0.0, float(width_mm))
        if width_value <= 0.0:
            return 0.0, 0.0
        return cls._fit_size_inside(
            width_value,
            width_value / ratio,
            max_width_mm,
            max_height_mm,
        )

    @classmethod
    def _aspect_size_from_height(
        cls,
        height_mm: float,
        aspect_ratio: float,
        max_width_mm: float = 99.0,
        max_height_mm: float = 99.0,
    ) -> tuple[float, float]:
        ratio = max(1e-9, float(aspect_ratio))
        height_value = max(0.0, float(height_mm))
        if height_value <= 0.0:
            return 0.0, 0.0
        return cls._fit_size_inside(
            height_value * ratio,
            height_value,
            max_width_mm,
            max_height_mm,
        )

    def _set_target_dimensions(self, width_mm: float, height_mm: float) -> None:
        self.size_spin.blockSignals(True)
        self.target_height_spin.blockSignals(True)
        self.size_spin.setValue(max(0.0, min(99.0, float(width_mm))))
        self.target_height_spin.setValue(
            max(0.0, min(99.0, float(height_mm)))
        )
        self.size_spin.blockSignals(False)
        self.target_height_spin.blockSignals(False)

    def _available_target_size(self) -> tuple[float, float]:
        return (
            max(0.0, 99.0 - float(self.offset_x_spin.value())),
            max(0.0, 99.0 - float(self.offset_y_spin.value())),
        )

    def _sync_locked_height(self, value: float) -> None:
        ratio = self._source_aspect_ratio()
        if (
            ratio is None
            or not self.lock_aspect_check.isChecked()
            or self.auto_size_check.isChecked()
        ):
            return
        width_mm, height_mm = self._aspect_size_from_width(
            float(value),
            ratio,
            *self._available_target_size(),
        )
        self._set_target_dimensions(width_mm, height_mm)

    def _sync_locked_width(self, value: float) -> None:
        ratio = self._source_aspect_ratio()
        if (
            ratio is None
            or not self.lock_aspect_check.isChecked()
            or self.auto_size_check.isChecked()
        ):
            return
        width_mm, height_mm = self._aspect_size_from_height(
            float(value),
            ratio,
            *self._available_target_size(),
        )
        self._set_target_dimensions(width_mm, height_mm)

    def _refresh_target_size_for_source(self, *_args) -> None:
        if self.auto_size_check.isChecked():
            self._apply_auto_size_if_needed()
        elif self.lock_aspect_check.isChecked():
            self._sync_locked_height(self.size_spin.value())

    def _apply_auto_size_if_needed(self, *_args) -> None:
        if not self.auto_size_check.isChecked() or self._source_image.isNull():
            return
        dpi = max(1, self.dpi_spin.value())
        physical_width = self._source_image.width() / dpi * 25.4
        physical_height = self._source_image.height() / dpi * 25.4
        width_mm, height_mm = self._fit_size_inside(
            physical_width,
            physical_height,
            *self._available_target_size(),
        )
        self._set_target_dimensions(width_mm, height_mm)

    def _fitted_mark_size_mm(self, image: QImage | None = None) -> tuple[float, float]:
        source = image if image is not None else self._source_image
        if source.isNull() or source.width() <= 0 or source.height() <= 0:
            return 0.0, 0.0
        target_width = float(self.size_spin.value())
        target_height = float(self.target_height_spin.value())
        if target_width <= 0.0 or target_height <= 0.0:
            return 0.0, 0.0
        if (
            not self.lock_aspect_check.isChecked()
            and not self.auto_size_check.isChecked()
        ):
            return target_width, target_height
        scale = min(
            target_width / source.width(),
            target_height / source.height(),
        )
        return source.width() * scale, source.height() * scale

    def _scale_for_scan_quality(
        self,
        source: QImage,
        transform_mode: Qt.TransformationMode,
        *,
        quality_lines_mm: float | None = None,
    ) -> QImage:
        actual_width, actual_height = self._fitted_mark_size_mm(source)
        quality = (
            self.scan_quality_spin.value()
            if quality_lines_mm is None
            else max(1.0, float(quality_lines_mm))
        )
        width_px = max(1, round(actual_width * quality))
        height_px = max(1, round(actual_height * quality))
        return source.scaled(
            width_px,
            height_px,
            Qt.AspectRatioMode.IgnoreAspectRatio,
            transform_mode,
        )

    def _scale_for_vector_quality(
        self,
        source: QImage,
        transform_mode: Qt.TransformationMode,
        *,
        preview: bool = False,
    ) -> QImage:
        actual_width, actual_height = self._fitted_mark_size_mm(source)
        quality = self.vector_quality_spin.value()
        if self.adaptive_quality_check.isChecked():
            longest_mm = max(actual_width, actual_height)
            quality *= max(0.65, min(1.5, 50.0 / max(1.0, longest_mm)))
        width_px = max(1, round(actual_width * quality))
        height_px = max(1, round(actual_height * quality))
        if preview and max(width_px, height_px) > 1024:
            factor = 1024.0 / max(width_px, height_px)
            width_px = max(1, round(width_px * factor))
            height_px = max(1, round(height_px * factor))
        return source.scaled(
            width_px,
            height_px,
            Qt.AspectRatioMode.IgnoreAspectRatio,
            transform_mode,
        )

    def _grayscale_rows_from_image(
        self,
        image: QImage,
        *,
        formula: str | None = None,
        red_weight: float | None = None,
        green_weight: float | None = None,
        blue_weight: float | None = None,
        invert: bool | None = None,
    ) -> list[bytearray]:
        selected_formula = (
            self.grayscale_formula_combo.currentData()
            if formula is None
            else formula
        )
        selected_red = (
            self.grayscale_red_spin.value()
            if red_weight is None
            else red_weight
        )
        selected_green = (
            self.grayscale_green_spin.value()
            if green_weight is None
            else green_weight
        )
        selected_blue = (
            self.grayscale_blue_spin.value()
            if blue_weight is None
            else blue_weight
        )
        selected_invert = (
            self.invert_image_check.isChecked()
            if invert is None
            else invert
        )
        return self._grayscale_rows_from_image_values(
            image,
            formula=str(selected_formula),
            red_weight=float(selected_red),
            green_weight=float(selected_green),
            blue_weight=float(selected_blue),
            invert=bool(selected_invert),
        )

    @staticmethod
    def _grayscale_rows_from_image_values(
        image: QImage,
        *,
        formula: str,
        red_weight: float,
        green_weight: float,
        blue_weight: float,
        invert: bool,
        cancel_check: Callable[[], None] | None = None,
    ) -> list[bytearray]:
        weights = grayscale_channel_weights(
            formula,
            red_weight=red_weight,
            green_weight=green_weight,
            blue_weight=blue_weight,
        )
        pixels = image.convertToFormat(QImage.Format.Format_RGBA8888)
        data = memoryview(pixels.constBits()).cast("B")
        stride = pixels.bytesPerLine()
        width = pixels.width()
        red_lut = tuple(value * weights[0] for value in range(256))
        green_lut = tuple(value * weights[1] for value in range(256))
        blue_lut = tuple(value * weights[2] for value in range(256))
        clamp_high = sum(weights) > 1.0
        rows: list[bytearray] = []
        for y in range(pixels.height()):
            if cancel_check is not None and y % 16 == 0:
                cancel_check()
            row = bytearray(width)
            index = y * stride
            for x in range(width):
                red = data[index]
                green = data[index + 1]
                blue = data[index + 2]
                alpha = data[index + 3]
                if alpha < 255:
                    opacity = alpha / 255.0
                    red = round(red * opacity + 255.0 * (1.0 - opacity))
                    green = round(green * opacity + 255.0 * (1.0 - opacity))
                    blue = round(blue * opacity + 255.0 * (1.0 - opacity))
                gray = round(
                    red_lut[red]
                    + green_lut[green]
                    + blue_lut[blue]
                )
                if clamp_high and gray > 255:
                    gray = 255
                row[x] = 255 - gray if invert else gray
                index += 4
            rows.append(row)
        return rows

    def get_gcode_bytes(self) -> bytes:
        if not self._editor_has_full_content:
            return self._gcode_data
        text = self.editor.toPlainText().replace("\r\n", "\n").replace("\r", "\n")
        self._gcode_data = text.encode("utf-8")
        return self._gcode_data

    def get_path(self) -> str:
        return self._path

    def set_content(
        self,
        path: str,
        data: bytes,
        *,
        render_preview: bool = False,
    ) -> None:
        self._path = path
        self._gcode_data = bytes(data)
        self._editor_has_full_content = len(data) <= EDITOR_EAGER_LOAD_MAX_BYTES
        display_data = data
        if not self._editor_has_full_content:
            display_data = data[:EDITOR_PREVIEW_MAX_BYTES]
            newline = display_data.rfind(b"\n")
            if newline >= 0:
                display_data = display_data[: newline + 1]
        highlight = (
            self._editor_has_full_content
            and len(display_data) <= EDITOR_HIGHLIGHT_MAX_BYTES
        )
        if self.highlighter.document() is not None:
            self.highlighter.setDocument(None)
        self.editor.blockSignals(True)
        self.editor.setUpdatesEnabled(False)
        self.editor.setPlainText(display_data.decode("utf-8", errors="replace"))
        self.editor.setReadOnly(not self._editor_has_full_content)
        self.editor.setUpdatesEnabled(True)
        self.editor.blockSignals(False)
        self.btn_load_full_gcode.setVisible(not self._editor_has_full_content)
        self.btn_adjust_gcode.setEnabled(
            bool(data) and self._gcode_adjust_task is None
        )
        if highlight:
            self.highlighter.setDocument(self.editor.document())
        self.lbl_file.setText(path or "未加载文件")
        self._update_size(
            len(data),
            highlight=highlight,
            preview_only=not self._editor_has_full_content,
        )
        self.gcode_loaded.emit(path, data)
        if render_preview:
            self._start_gcode_preview(path, data)

    def _start_gcode_preview(self, path: str, data: bytes) -> None:
        if self._gcode_preview_task is not None:
            self._gcode_preview_task.cancel()
        self._gcode_preview_serial += 1
        serial = self._gcode_preview_serial
        task = _GcodePreviewTask(serial, path, data)
        self._gcode_preview_task = task
        self._gcode_preview_tasks.add(task)
        task.signals.ready.connect(self._gcode_preview_ready)
        task.signals.failed.connect(self._gcode_preview_failed)
        task.signals.finished.connect(self._gcode_preview_finished)

        self._showing_gcode_preview = True
        self._image_path = ""
        self._original_image = QImage()
        self._source_image = QImage()
        self._converted_image = QImage()
        self._source_status = ""
        self._converted_status = ""
        self._preview_mode = "converted"
        self.btn_preview_original.setText("无原图")
        self.btn_preview_original.setEnabled(False)
        self.btn_preview_original.setChecked(False)
        self.btn_preview_converted.setText("G-code预览")
        self.btn_preview_converted.setEnabled(False)
        self.btn_preview_converted.setChecked(False)
        self.preview_label.clear()
        self.preview_label.setText("正在解析成品 G-code 的打标轨迹…")
        self.image_status.setText(
            "正在后台还原 G-code 图像；仅绘制激光开启且 S>0 的 G1 轨迹"
        )
        self.tabs.setCurrentIndex(0)
        self._gcode_preview_pool.start(task)

    def _abandon_gcode_preview(self) -> None:
        self._gcode_preview_serial += 1
        if self._gcode_preview_task is not None:
            self._gcode_preview_task.cancel()
        self._gcode_preview_task = None
        self._showing_gcode_preview = False

    def _gcode_preview_ready(
        self,
        serial: int,
        path: str,
        byte_count: int,
        result: object,
    ) -> None:
        if serial != self._gcode_preview_serial or self._gcode_preview_task is None:
            return
        assert isinstance(result, GcodePreviewResult)
        self._gcode_preview_task = None
        name = Path(path).name if path else "编辑器 G-code"
        if result.geometry is None or result.segment_count <= 0:
            self._converted_image = QImage()
            self.btn_preview_converted.setEnabled(False)
            self.preview_label.clear()
            self.preview_label.setText(
                "此 G-code 没有可显示的打标图像\n\n"
                "未发现 M3/M4 开光、S>0 且由 G1 形成的有效轨迹"
            )
            self._converted_status = (
                f"{name} · 已解析 {result.line_count} 行 · 0 条有效打标轨迹 · "
                f"{byte_count} bytes"
            )
            self.image_status.setText(self._converted_status)
            return

        geometry = result.geometry
        self._converted_image = result.image
        self._converted_status = (
            f"{name} · G-code 实际打标轨迹 · 图形 "
            f"{geometry.width:.2f}×{geometry.height:.2f} mm · "
            f"X {geometry.min_x:.2f}-{geometry.max_x:.2f} · "
            f"Y {geometry.min_y:.2f}-{geometry.max_y:.2f}\n"
            f"{result.segment_count} 条开光 G1 线段 · 最大功率 S{result.max_power_s:g} · "
            f"G0/S0/关光段未绘制 · {byte_count} bytes"
        )
        self.btn_preview_converted.setEnabled(True)
        self._set_preview_mode("converted")

    def _gcode_preview_failed(self, serial: int, message: str) -> None:
        if serial != self._gcode_preview_serial:
            return
        self._gcode_preview_task = None
        self._converted_image = QImage()
        self.btn_preview_converted.setEnabled(False)
        self.preview_label.clear()
        self.preview_label.setText("G-code 已打开，但轨迹预览生成失败")
        self.image_status.setText(f"G-code 轨迹预览失败：{message}")

    def _gcode_preview_finished(self, task: object) -> None:
        if not isinstance(task, _GcodePreviewTask):
            return
        self._gcode_preview_tasks.discard(task)
        QTimer.singleShot(
            0,
            lambda current=task: self._finish_orphaned_gcode_preview(current),
        )

    def _finish_orphaned_gcode_preview(self, task: _GcodePreviewTask) -> None:
        if self._gcode_preview_task is not task:
            return
        self._gcode_preview_task = None
        self.btn_preview_converted.setEnabled(False)
        self.preview_label.clear()
        self.preview_label.setText("G-code 已打开，但后台预览任务没有返回结果")
        self.image_status.setText("G-code 轨迹预览任务已结束，请重新打开文件")

    def _update_gcode_adjust_action(self) -> None:
        self.btn_adjust_gcode.setEnabled(
            self._gcode_adjust_task is None
            and (
                self.editor.document().characterCount() > 1
                or bool(self._gcode_data)
            )
        )

    def _handle_gcode_adjust_action(self) -> None:
        if self._gcode_adjust_task is not None:
            self._gcode_adjust_serial += 1
            self._gcode_adjust_task.cancel()
            self._gcode_adjust_task = None
            self._set_gcode_adjust_busy(False)
            self.lbl_file.setText("已取消 G-code 处理；后台正在结束")
            return
        data = self.get_gcode_bytes()
        if not data:
            QMessageBox.information(self, "调整 G-code", "请先打开或粘贴 G-code")
            return
        self._start_gcode_analysis(
            self._path,
            data.decode("utf-8", errors="replace"),
        )

    def _set_gcode_adjust_busy(self, busy: bool) -> None:
        if busy:
            self._gcode_adjust_started_at = monotonic()
            self._gcode_adjust_timer.start()
        else:
            self._gcode_adjust_timer.stop()
        self.btn_adjust_gcode.setText("取消调整" if busy else "调整参数…")
        self.btn_adjust_gcode.setObjectName(
            "btnDanger" if busy else "btnGcodeAdjust"
        )
        self.btn_adjust_gcode.style().unpolish(self.btn_adjust_gcode)
        self.btn_adjust_gcode.style().polish(self.btn_adjust_gcode)
        self.btn_adjust_gcode.setEnabled(
            busy
            or self.editor.document().characterCount() > 1
            or bool(self._gcode_data)
        )
        self.btn_open.setEnabled(not busy)
        self.btn_clear.setEnabled(not busy)
        self.btn_load_full_gcode.setEnabled(not busy)
        self.editor.setReadOnly(busy or not self._editor_has_full_content)

    def _refresh_gcode_adjust_status(self) -> None:
        if self._gcode_adjust_task is None:
            return
        elapsed = max(0.0, monotonic() - self._gcode_adjust_started_at)
        self.lbl_file.setText(
            f"{self._gcode_adjust_phase} 已用 {elapsed:.1f} 秒（可点击取消调整）"
        )

    def _start_gcode_analysis(self, path: str, source_text: str) -> None:
        self._gcode_adjust_serial += 1
        serial = self._gcode_adjust_serial
        task = _GcodeAnalysisTask(serial, path, source_text)
        self._gcode_adjust_task = task
        self._gcode_adjust_tasks.add(task)
        task.signals.ready.connect(self._gcode_analysis_ready)
        task.signals.failed.connect(self._gcode_adjustment_failed)
        task.signals.finished.connect(self._gcode_adjustment_finished)
        self._gcode_adjust_phase = "正在分析 G-code…"
        self._set_gcode_adjust_busy(True)
        self._refresh_gcode_adjust_status()
        self._gcode_adjust_pool.start(task)

    def _gcode_analysis_ready(
        self,
        serial: int,
        path: str,
        source_text: str,
        stats: object,
    ) -> None:
        if serial != self._gcode_adjust_serial or self._gcode_adjust_task is None:
            return
        assert isinstance(stats, GcodeTransformStats)
        self._gcode_adjust_task = None
        self._set_gcode_adjust_busy(False)
        self._open_gcode_adjust_dialog(
            source_text=source_text,
            stats=stats,
        )

    def _start_gcode_adjustment(
        self,
        path: str,
        source_text: str,
        options: GcodeTransformOptions,
    ) -> None:
        self._gcode_adjust_serial += 1
        serial = self._gcode_adjust_serial
        task = _GcodeAdjustmentTask(serial, path, source_text, options)
        self._gcode_adjust_task = task
        self._gcode_adjust_tasks.add(task)
        task.signals.ready.connect(self._gcode_adjustment_ready)
        task.signals.failed.connect(self._gcode_adjustment_failed)
        task.signals.finished.connect(self._gcode_adjustment_finished)
        self._gcode_adjust_phase = "正在调整并校验 G-code…"
        self._set_gcode_adjust_busy(True)
        self._refresh_gcode_adjust_status()
        self._gcode_adjust_pool.start(task)

    def _gcode_adjustment_ready(
        self,
        serial: int,
        path: str,
        result: object,
    ) -> None:
        if serial != self._gcode_adjust_serial or self._gcode_adjust_task is None:
            return
        assert isinstance(result, GcodeTransformResult)
        self._gcode_adjust_task = None
        try:
            adjusted_data = result.text.encode("utf-8")
            self.set_content(path, adjusted_data, render_preview=True)
            self.lbl_file.setText(f"{path} · 已调整")
            self.lbl_file.setToolTip(
                f"功率上限 S{result.after.max_power_s:g} · "
                f"速度上限 F{result.after.max_feed_mm_min:g}"
            )
        finally:
            self._set_gcode_adjust_busy(False)

    def _gcode_adjustment_failed(self, serial: int, message: str) -> None:
        if serial != self._gcode_adjust_serial:
            return
        self._gcode_adjust_task = None
        self._set_gcode_adjust_busy(False)
        QMessageBox.warning(self, "G-code 调整失败", message)

    def _gcode_adjustment_finished(self, task: object) -> None:
        if not isinstance(task, (_GcodeAdjustmentTask, _GcodeAnalysisTask)):
            return
        self._gcode_adjust_tasks.discard(task)
        QTimer.singleShot(
            0,
            lambda current=task: self._finish_orphaned_gcode_adjustment(current),
        )

    def _finish_orphaned_gcode_adjustment(
        self,
        task: _GcodeAdjustmentTask | _GcodeAnalysisTask,
    ) -> None:
        if self._gcode_adjust_task is not task:
            return
        self._gcode_adjust_task = None
        self._set_gcode_adjust_busy(False)
        self.lbl_file.setText("G-code 调整任务已结束，但没有返回可用结果")

    @staticmethod
    def _adjusted_gcode_path(path: str) -> str:
        if not path:
            return "editor.adjusted.gcode"
        source = Path(path)
        suffix = source.suffix or ".gcode"
        stem = source.stem
        if stem.endswith(".adjusted"):
            stem = stem[: -len(".adjusted")]
        return str(source.with_name(f"{stem}.adjusted{suffix}"))

    def _open_gcode_adjust_dialog(
        self,
        *,
        source_text: str | None = None,
        stats: GcodeTransformStats | None = None,
    ) -> None:
        if source_text is None or stats is None:
            data = self.get_gcode_bytes()
            if not data:
                QMessageBox.information(self, "调整 G-code", "请先打开或粘贴 G-code")
                return
            source_text = data.decode("utf-8", errors="replace")
            try:
                stats = analyze_gcode(source_text)
            except Exception as exc:
                QMessageBox.warning(self, "无法分析 G-code", str(exc))
                return
        assert source_text is not None
        assert stats is not None

        dialog = QDialog(self)
        dialog.setWindowTitle("成品 G-code 参数调整")
        dialog.setMinimumSize(600, 500)
        dialog.setStyleSheet(self._lasergrbl_dialog_stylesheet())
        layout = QVBoxLayout(dialog)
        layout.setContentsMargins(14, 14, 14, 14)
        layout.setSpacing(12)

        current_summary = QLabel(dialog)
        current_summary.setObjectName("gcodeAdjustCurrentSummary")
        current_summary.setWordWrap(True)
        current_summary.setStyleSheet(
            "color:#475569; background:#f8fafc; border:1px solid #e2e8f0; "
            "border-radius:7px; padding:9px;"
        )
        if stats.geometry is None:
            geometry_summary = "未检测到 X/Y 轨迹"
        else:
            geometry_summary = (
                f"范围 X {stats.geometry.min_x:.3f}-{stats.geometry.max_x:.3f} mm · "
                f"Y {stats.geometry.min_y:.3f}-{stats.geometry.max_y:.3f} mm · "
                f"尺寸 {stats.geometry.width:.3f}×{stats.geometry.height:.3f} mm"
            )
        current_summary.setText(
            f"当前文件：{stats.line_count} 行 · 最大功率 S{stats.max_power_s:g} · "
            f"最大速度 F{stats.max_feed_mm_min:g}\n{geometry_summary}"
        )
        layout.addWidget(current_summary)

        process_group = QGroupBox("功率与速度", dialog)
        process_form = QFormLayout(process_group)
        process_form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft)
        process_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow
        )
        process_form.setHorizontalSpacing(18)
        process_form.setVerticalSpacing(10)
        layout.addWidget(process_group)

        target_power = QSpinBox(dialog)
        target_power.setObjectName("gcodeAdjustMaxPower")
        target_power.setRange(0, 1000)
        target_power.setSingleStep(10)
        target_power.setValue(max(0, min(1000, round(stats.max_power_s))))
        target_power.setEnabled(stats.max_power_s > 0.0)
        target_power.setSuffix(" S")
        process_form.addRow("目标最大功率", target_power)

        target_feed = QSpinBox(dialog)
        target_feed.setObjectName("gcodeAdjustMaxFeed")
        target_feed.setRange(1, 100_000)
        target_feed.setSingleStep(100)
        target_feed.setValue(
            max(1, min(100_000, round(stats.max_feed_mm_min or 1.0)))
        )
        target_feed.setEnabled(stats.max_feed_mm_min > 0.0)
        target_feed.setSuffix(" mm/min")
        process_form.addRow("目标最大速度", target_feed)

        process_hint = QLabel(
            "功率和速度按当前最大值等比例调整，灰度 PWM 和多级速度之间的比例保持不变。",
            process_group,
        )
        process_hint.setWordWrap(True)
        process_hint.setStyleSheet("color:#64748b; font-size:11px;")
        process_form.addRow("调整方式", process_hint)

        geometry_group = QGroupBox("等比缩放与整体偏移 [mm]", dialog)
        geometry_form = QFormLayout(geometry_group)
        geometry_form.setLabelAlignment(Qt.AlignmentFlag.AlignLeft)
        geometry_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow
        )
        geometry_form.setHorizontalSpacing(18)
        geometry_form.setVerticalSpacing(10)
        layout.addWidget(geometry_group)

        scale = QDoubleSpinBox(dialog)
        scale.setObjectName("gcodeAdjustScalePercent")
        scale.setRange(1.0, 1000.0)
        scale.setDecimals(1)
        scale.setSingleStep(1.0)
        scale.setValue(100.0)
        scale.setSuffix(" %")
        scale.setEnabled(stats.geometry is not None)
        geometry_form.addRow("等比缩放", scale)

        offset_row = QWidget(geometry_group)
        offset_layout = QHBoxLayout(offset_row)
        offset_layout.setContentsMargins(0, 0, 0, 0)
        offset_layout.setSpacing(12)
        offset_x = QDoubleSpinBox(dialog)
        offset_x.setObjectName("gcodeAdjustOffsetX")
        offset_x.setRange(-99.0, 99.0)
        offset_x.setDecimals(1)
        offset_x.setSingleStep(0.1)
        offset_x.setPrefix("X: ")
        offset_x.setSuffix(" mm")
        offset_y = QDoubleSpinBox(dialog)
        offset_y.setObjectName("gcodeAdjustOffsetY")
        offset_y.setRange(-99.0, 99.0)
        offset_y.setDecimals(1)
        offset_y.setSingleStep(0.1)
        offset_y.setPrefix("Y: ")
        offset_y.setSuffix(" mm")
        for widget in (offset_x, offset_y):
            widget.setEnabled(stats.geometry is not None)
            offset_layout.addWidget(widget, 1)
        geometry_form.addRow("移动量", offset_row)

        result_summary = QLabel(dialog)
        result_summary.setObjectName("gcodeAdjustResultSummary")
        result_summary.setWordWrap(True)
        result_summary.setStyleSheet(
            "color:#0284c7; background:#f0f9ff; border:1px solid #bae6fd; "
            "border-radius:7px; padding:9px; font-weight:600;"
        )
        layout.addWidget(result_summary)
        layout.addStretch(1)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel,
            parent=dialog,
        )
        apply_button = buttons.button(QDialogButtonBox.StandardButton.Ok)
        apply_button.setText("应用调整")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("取消")
        layout.addWidget(buttons)
        numeric_inputs = self._configure_dialog_number_inputs(dialog)

        def update_result_summary() -> None:
            if stats.geometry is None:
                result_summary.setText("当前文件没有可缩放或偏移的 X/Y 轨迹，可只调整功率和速度。")
                apply_button.setEnabled(True)
                return
            factor = scale.value() / 100.0
            min_x = stats.geometry.min_x + offset_x.value()
            min_y = stats.geometry.min_y + offset_y.value()
            max_x = min_x + stats.geometry.width * factor
            max_y = min_y + stats.geometry.height * factor
            valid = min_x >= 0.0 and min_y >= 0.0 and max_x <= 99.0 and max_y <= 99.0
            geometry_requested = (
                abs(factor - 1.0) > 1e-12
                or abs(offset_x.value()) > 1e-12
                or abs(offset_y.value()) > 1e-12
            )
            unsupported = geometry_requested and (
                stats.has_coordinate_reset
                or stats.has_homing
                or (
                    (abs(offset_x.value()) > 1e-12 or abs(offset_y.value()) > 1e-12)
                    and not stats.has_absolute_coordinates
                )
            )
            if unsupported:
                result_summary.setText(
                    "该任务使用 G92/G28 或纯相对坐标，不能安全应用当前几何调整；"
                    "功率和速度仍可单独调整。"
                )
                apply_button.setEnabled(False)
                return
            result_summary.setText(
                f"调整后范围：X {min_x:.3f}-{max_x:.3f} mm · "
                f"Y {min_y:.3f}-{max_y:.3f} mm · "
                f"尺寸 {max_x - min_x:.3f}×{max_y - min_y:.3f} mm"
                + (" · 范围有效" if valid else " · 超出 99×99 mm 工作区")
            )
            apply_button.setEnabled(valid)

        for widget in (scale, offset_x, offset_y):
            widget.valueChanged.connect(lambda *_: update_result_summary())
        update_result_summary()

        def accept() -> None:
            self._commit_dialog_number_inputs(numeric_inputs)
            update_result_summary()
            if not apply_button.isEnabled():
                return
            adjusted_path = self._adjusted_gcode_path(self._path)
            options = GcodeTransformOptions(
                target_max_power_s=(
                    target_power.value() if target_power.isEnabled() else None
                ),
                target_max_feed_mm_min=(
                    target_feed.value() if target_feed.isEnabled() else None
                ),
                scale_percent=scale.value(),
                offset_x_mm=offset_x.value(),
                offset_y_mm=offset_y.value(),
            )
            dialog.accept()
            self._start_gcode_adjustment(adjusted_path, source_text, options)

        buttons.accepted.connect(accept)
        buttons.rejected.connect(dialog.reject)
        dialog.exec()

    def _load_full_gcode_into_editor(self) -> None:
        if self._editor_has_full_content:
            return
        if self.highlighter.document() is not None:
            self.highlighter.setDocument(None)
        self.editor.blockSignals(True)
        self.editor.setUpdatesEnabled(False)
        self.editor.setPlainText(self._gcode_data.decode("utf-8", errors="replace"))
        self.editor.setReadOnly(False)
        self.editor.setUpdatesEnabled(True)
        self.editor.blockSignals(False)
        self._editor_has_full_content = True
        self.btn_load_full_gcode.hide()
        highlight = len(self._gcode_data) <= EDITOR_HIGHLIGHT_MAX_BYTES
        if highlight:
            self.highlighter.setDocument(self.editor.document())
        self._update_size(len(self._gcode_data), highlight=highlight)

    def _apply_crop(self, rect: QRect) -> None:
        if self._source_image.isNull():
            return
        crop_rect = rect.normalized().intersected(self._source_image.rect())
        if crop_rect.isEmpty():
            raise ValueError("裁剪区域不能为空")
        if crop_rect == self._source_image.rect():
            return
        previous_size = self._source_image.size()
        self._source_image = self._source_image.copy(crop_rect)
        self._converted_image = QImage()
        self._source_status = (
            f"✓ 裁剪已生效 · {Path(self._image_path).name if self._image_path else '生成图像'} · "
            f"{previous_size.width()}×{previous_size.height()} → "
            f"{self._source_image.width()}×{self._source_image.height()} px"
        )
        self.btn_preview_original.setText("裁切后")
        self.btn_preview_converted.setText("转换效果")
        self._converted_status = ""
        self.btn_preview_converted.setEnabled(False)
        self._refresh_target_size_for_source()
        self._set_preview_mode("original")

    def _restore_original_image(self) -> None:
        if self._original_image.isNull():
            return
        self._source_image = self._original_image.copy()
        self._converted_image = QImage()
        self._source_status = (
            f"原图 · {Path(self._image_path).name if self._image_path else '生成图像'} · "
            f"{self._source_image.width()}×{self._source_image.height()} px"
        )
        self.btn_preview_original.setText("原图")
        self.btn_preview_converted.setText("转换效果")
        self._converted_status = ""
        self.btn_preview_converted.setEnabled(False)
        self._refresh_target_size_for_source()
        self._set_preview_mode("original")

    def set_image(self, path: str, image: QImage) -> None:
        if image.isNull():
            raise ValueError("无法解码图像")
        self._abandon_gcode_preview()
        self._image_path = path
        self._original_image = image.copy()
        self._source_image = image.copy()
        self._converted_image = QImage()
        self._source_status = (
            f"原图 · {Path(path).name if path else '生成图像'} · "
            f"{image.width()}×{image.height()} px"
        )
        self.btn_preview_original.setText("原图")
        self.btn_preview_converted.setText("转换效果")
        self._refresh_target_size_for_source()
        self._converted_status = ""
        self.btn_preview_original.setEnabled(True)
        self.btn_preview_converted.setEnabled(False)
        self._set_preview_mode("original")

    def show_ai_unavailable(self) -> None:
        self.image_status.setText("未配置 AI 图像服务；可使用“导入图片”完成本地转换")

    def update_ui_generating_state(self, is_generating: bool) -> None:
        self._is_generating = is_generating
        self.btn_generate.setEnabled(True)
        self.btn_generate.setText("取消生成" if is_generating else "生成图像")
        self.btn_generate.setObjectName("btnDanger" if is_generating else "btnPrimary")
        self.btn_generate.style().unpolish(self.btn_generate)
        self.btn_generate.style().polish(self.btn_generate)
        self.btn_import_image.setEnabled(not is_generating)
        self.btn_output_dir.setEnabled(not is_generating)
        self.prompt_edit.setEnabled(not is_generating)
        self.prompt_preset_combo.setEnabled(not is_generating)
        self.image_size_combo.setEnabled(not is_generating)
        self.watermark_check.setEnabled(not is_generating)
        self.output_dir_edit.setEnabled(not is_generating)
        if is_generating:
            self.image_status.setText("生成中...")

    def _handle_ai_generation_action(self) -> None:
        if self._is_generating:
            self.btn_generate.setEnabled(False)
            self.btn_generate.setText("正在取消...")
            self.image_status.setText("正在取消豆包生图...")
            self.ai_generation_cancel_requested.emit()
            return
        self._request_ai_image()

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
            "图像文件 (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;所有文件 (*.*)",
        )
        if path:
            image = QImage(path)
            if image.isNull():
                self.image_status.setText("图像读取失败")
                return
            self.set_image(path, image)

    def _handle_conversion_action(self) -> None:
        if self._conversion_task is not None:
            self._conversion_serial += 1
            self._conversion_task.cancel()
            self._conversion_task = None
            self._set_conversion_busy(False)
            self.image_status.setText("已取消转换；正在结束后台计算")
            return
        if self._source_image.isNull():
            self.image_status.setText("请先生成或导入图像")
            return

        mode = str(self.extract_mode.currentData())
        if mode not in RASTER_EXTRACTION_MODES:
            # Vector extraction is retained on its existing path for now; the
            # expensive high-density raster path uses the cancellable worker.
            self._convert_image()
            return

        # Unit/contract callers traditionally click an unshown page and expect
        # immediate results.  A real user cannot click a hidden page, so keep
        # that deterministic API while all visible UI conversions are async.
        if not self.isVisible():
            self._convert_image()
            return

        request = self._capture_raster_conversion_request()
        self._conversion_serial += 1
        serial = self._conversion_serial
        task = _ConversionTask(serial, request, self._compute_raster_conversion)
        self._conversion_task = task
        self._conversion_tasks.add(task)
        task.signals.progress.connect(self._conversion_progress)
        task.signals.ready.connect(self._conversion_ready)
        task.signals.failed.connect(self._conversion_failed)
        task.signals.finished.connect(self._conversion_task_finished)
        self._set_conversion_busy(True)
        self._conversion_phase = "正在准备图像转换..."
        self._refresh_conversion_progress_status()
        self._conversion_pool.start(task)

    def _set_conversion_busy(self, busy: bool) -> None:
        if busy:
            self._conversion_started_at = monotonic()
            self._conversion_status_timer.start()
        else:
            self._conversion_status_timer.stop()
        self.btn_convert.setText("取消转换" if busy else "转换为 G-code")
        self.btn_convert.setObjectName("btnDanger" if busy else "btnAccent")
        self.btn_convert.style().unpolish(self.btn_convert)
        self.btn_convert.style().polish(self.btn_convert)
        self.btn_image_params.setEnabled(not busy)
        self.btn_target_params.setEnabled(not busy)
        self.btn_import_image.setEnabled(not busy and not self._is_generating)

    def _conversion_progress(self, serial: int, message: str) -> None:
        if serial == self._conversion_serial and self._conversion_task is not None:
            self._conversion_phase = message
            self._refresh_conversion_progress_status()

    def _refresh_conversion_progress_status(self) -> None:
        if self._conversion_task is None:
            return
        elapsed = max(0.0, monotonic() - self._conversion_started_at)
        phase = self._conversion_phase or "正在转换图像..."
        self.image_status.setText(
            f"{phase}（已用 {elapsed:.1f} 秒，可点击“取消转换”终止）"
        )

    def _conversion_ready(self, serial: int, result: object) -> None:
        if serial != self._conversion_serial or self._conversion_task is None:
            return
        assert isinstance(result, _ConversionResult)
        elapsed = max(0.0, monotonic() - self._conversion_started_at)
        try:
            self._converted_image = result.preview
            self._converted_status = f"{result.status} · 转换耗时 {elapsed:.2f} 秒"
            self.set_content(result.path, result.data)
            self.btn_preview_converted.setEnabled(True)
            self._set_preview_mode("converted")
        except Exception as exc:
            self.image_status.setText(f"转换结果装载失败：{exc}")
        finally:
            if serial == self._conversion_serial:
                self._conversion_task = None
                self._set_conversion_busy(False)

    def _conversion_failed(self, serial: int, message: str) -> None:
        if serial != self._conversion_serial:
            return
        self._conversion_task = None
        self._set_conversion_busy(False)
        self.image_status.setText(f"转换失败：{message}")

    def _conversion_task_finished(self, task: object) -> None:
        if isinstance(task, _ConversionTask):
            self._conversion_tasks.discard(task)
            QTimer.singleShot(0, lambda current=task: self._finish_orphaned_conversion(current))

    def _finish_orphaned_conversion(self, task: _ConversionTask) -> None:
        if self._conversion_task is not task:
            return
        self._conversion_task = None
        self._set_conversion_busy(False)
        self.image_status.setText(
            "转换后台任务已结束，但没有返回可用结果；请重试或降低扫描质量"
        )

    def _capture_raster_conversion_request(self) -> _RasterConversionRequest:
        return _RasterConversionRequest(
            source_image=self._source_image.copy(),
            image_path=self._image_path,
            mode=str(self.extract_mode.currentData()),
            smooth_resize=self.resize_mode_combo.currentData() != "nearest",
            scan_quality=float(self.scan_quality_spin.value()),
            target_width_mm=float(self.size_spin.value()),
            target_height_mm=float(self.target_height_spin.value()),
            lock_aspect=(
                self.lock_aspect_check.isChecked()
                or self.auto_size_check.isChecked()
            ),
            offset_x=float(self.offset_x_spin.value()),
            offset_y=float(self.offset_y_spin.value()),
            grayscale_formula=str(self.grayscale_formula_combo.currentData()),
            red_weight=float(self.grayscale_red_spin.value()),
            green_weight=float(self.grayscale_green_spin.value()),
            blue_weight=float(self.grayscale_blue_spin.value()),
            invert=self.invert_image_check.isChecked(),
            brightness=self.lasergrbl_brightness_spin.value(),
            contrast=self.lasergrbl_contrast_spin.value(),
            white_clip=self.lasergrbl_white_clip_spin.value(),
            black_clip=self.lasergrbl_black_clip_spin.value(),
            laser_mode=str(self.laser_mode_combo.currentData()),
            min_power=min(self.power_min_spin.value(), self.power_spin.value()),
            mark_power=max(self.power_min_spin.value(), self.power_spin.value()),
            threshold=self.threshold_spin.value(),
            threshold_enabled=self.threshold_enabled_check.isChecked(),
            raster_pwm=self.raster_pwm_check.isChecked(),
            auto_line_art=self.auto_line_art_check.isChecked(),
            scan_direction=str(self.scan_direction_combo.currentData()),
            scan_direction_label=self.scan_direction_combo.currentText(),
            unidirectional=self.unidirectional_scan_check.isChecked(),
            rapid_white=self.rapid_white_check.isChecked(),
            dither_algorithm=str(self.dither_algorithm_combo.currentData()),
            dither_label=self.dither_algorithm_combo.currentText(),
            mark_speed=self.speed_spin.value(),
            fill_speed=self.fill_speed_spin.value(),
            min_area=(
                float(self.vector_min_area.value())
                if self.spot_removal_check.isChecked()
                else 1.0
            ),
            adaptive_quality=self.adaptive_quality_check.isChecked(),
            optimize_paths=self.optimize_paths_check.isChecked(),
        )

    @staticmethod
    def _compute_raster_conversion(
        request: _RasterConversionRequest,
        *,
        check_cancelled: Callable[[], None],
        progress: Callable[[str], None],
    ) -> _ConversionResult:
        source = request.source_image
        source_name = Path(request.image_path).stem if request.image_path else "generated_image"
        labels = {
            "scanline": ("线到线跟踪", "scanline"),
            "dither": ("1bit B/W抖动", "dither"),
            "centerline": ("中心线", "centerline"),
            "passthrough": ("直通", "passthrough"),
        }
        mode_label, suffix = labels[request.mode]
        target_width = request.target_width_mm
        target_height = request.target_height_mm
        if target_width <= 0.0 or target_height <= 0.0:
            data = f"{request.laser_mode} S0\nM5\n".encode("utf-8")
            preview = QImage(1, 1, QImage.Format.Format_RGB32)
            preview.fill(QColor("white"))
            return _ConversionResult(
                f"{source_name}.{suffix}.generated.gcode",
                data,
                preview,
                f"{mode_label} · 0×0 mm · 安全空任务，不生成移动或开光路径 · {len(data)} bytes",
            )

        if request.lock_aspect:
            scale_mm = min(
                target_width / source.width(),
                target_height / source.height(),
            )
            actual_width = source.width() * scale_mm
            actual_height = source.height() * scale_mm
        else:
            actual_width = target_width
            actual_height = target_height
        if (
            request.offset_x + actual_width > 99.0005
            or request.offset_y + actual_height > 99.0005
        ):
            raise ValueError(
                "目标图形超出 99×99 mm 工作区："
                f"X 至 {request.offset_x + actual_width:.1f} mm，"
                f"Y 至 {request.offset_y + actual_height:.1f} mm"
            )

        progress("正在按扫描质量缩放图像...")
        check_cancelled()
        width_px = max(1, round(actual_width * request.scan_quality))
        height_px = max(1, round(actual_height * request.scan_quality))
        image = source.scaled(
            width_px,
            height_px,
            Qt.AspectRatioMode.IgnoreAspectRatio,
            Qt.SmoothTransformation if request.smooth_resize else Qt.FastTransformation,
        )
        progress(f"正在处理 {width_px}×{height_px} 像素灰度数据...")
        rows = GcodePage._grayscale_rows_from_image_values(
            image,
            formula=request.grayscale_formula,
            red_weight=request.red_weight,
            green_weight=request.green_weight,
            blue_weight=request.blue_weight,
            invert=request.invert,
            cancel_check=check_cancelled,
        )
        tone_rows = apply_lasergrbl_tone(
            rows,
            brightness=request.brightness,
            contrast=request.contrast,
            white_clip=request.white_clip,
            black_clip=request.black_clip,
        )
        check_cancelled()

        def binary_job(mask: list[list[bool]]) -> str:
            binary_rows = [
                bytearray(0 if is_dark else 255 for is_dark in row)
                for row in mask
            ]
            return grayscale_rows_to_gcode(
                binary_rows,
                target_width,
                request.fill_speed,
                height_mm=target_height,
                power_min_s=0,
                power_max_s=request.mark_power,
                laser_mode=request.laser_mode,
                x_offset_mm=request.offset_x,
                y_offset_mm=request.offset_y,
                direction=request.scan_direction,
                unidirectional=request.unidirectional,
                rapid_white=request.rapid_white,
                cancel_check=check_cancelled,
            )

        path_count = 0
        point_count = 0
        auto_binary = False
        progress("正在生成紧凑 G-code 色段...")
        if request.mode == "dither":
            mask = dither_rows_to_mask(
                tone_rows,
                request.threshold,
                request.dither_algorithm,
            )
            check_cancelled()
            text = binary_job(mask)
            preview = GcodePage._build_mask_preview(
                GcodePage._limit_mask_preview(mask)
            )
            algorithm = f"{request.dither_label} 1bit"
        elif request.mode == "centerline":
            base_mask = prepare_laser_mask(
                tone_rows,
                request.threshold,
                adaptive=request.adaptive_quality,
                edge_enhance=False,
                min_area_px=request.min_area,
            )
            mask = centerline_mask(base_mask)
            check_cancelled()
            paths = trace_centerline_paths(mask)
            text = centerline_paths_to_gcode(
                paths,
                image.width(),
                image.height(),
                target_width,
                request.mark_speed,
                height_mm=target_height,
                power_s=request.mark_power,
                laser_mode=request.laser_mode,
                x_offset_mm=request.offset_x,
                y_offset_mm=request.offset_y,
                optimize_paths=request.optimize_paths,
                rapid_travel=request.rapid_white,
            )
            preview = GcodePage._build_mask_preview(
                GcodePage._limit_mask_preview(mask)
            )
            path_count = len(paths)
            point_count = sum(len(path) for path in paths)
            algorithm = "Zhang–Suen 细化 · 8 连通中心线路径"
        else:
            auto_binary = (
                request.auto_line_art
                and GcodePage._looks_like_binary_line_art(tone_rows)
            )
            use_binary = (
                request.threshold_enabled
                or not request.raster_pwm
                or auto_binary
            )
            if use_binary:
                mask = prepare_laser_mask(
                    tone_rows,
                    request.threshold,
                    adaptive=False,
                    edge_enhance=False,
                    min_area_px=0,
                )
                check_cancelled()
                text = binary_job(mask)
                preview = GcodePage._build_mask_preview(
                    GcodePage._limit_mask_preview(mask)
                )
                if auto_binary and not request.threshold_enabled:
                    algorithm = "自动识别黑白线稿 · 二值色段精简"
                else:
                    algorithm = (
                        "直通二值阈值"
                        if request.mode == "passthrough"
                        else "线到线 · 二值阈值"
                    )
            else:
                text = grayscale_rows_to_gcode(
                    tone_rows,
                    target_width,
                    request.fill_speed,
                    height_mm=target_height,
                    power_min_s=request.min_power,
                    power_max_s=request.mark_power,
                    laser_mode=request.laser_mode,
                    x_offset_mm=request.offset_x,
                    y_offset_mm=request.offset_y,
                    direction=request.scan_direction,
                    unidirectional=request.unidirectional,
                    rapid_white=request.rapid_white,
                    cancel_check=check_cancelled,
                )
                check_cancelled()
                preview = GcodePage._build_grayscale_tone_preview(
                    GcodePage._limit_preview_rows(tone_rows),
                    power_min_s=request.min_power,
                    power_max_s=request.mark_power,
                )
                algorithm = (
                    "直通灰度 PWM"
                    if request.mode == "passthrough"
                    else "线到线 · 8-bit 灰度 PWM"
                )

        progress("正在整理 G-code 与效果预览...")
        data = text.encode("utf-8")
        scan_line_count = {
            "vertical": image.width(),
            "diagonal": image.width() + image.height() - 1,
        }.get(request.scan_direction, image.height())
        threshold_summary = (
            f"阈值 {request.threshold}"
            if request.mode in ("dither", "centerline")
            or request.threshold_enabled
            or not request.raster_pwm
            or auto_binary
            else "灰度 PWM"
        )
        vector_stats = (
            f" · 路径 {path_count} · 节点 {point_count}"
            if request.mode == "centerline"
            else ""
        )
        status = (
            f"{mode_label} · {algorithm} · 图形 {actual_width:.1f}×{actual_height:.1f} mm · "
            f"X {request.offset_x:.1f}-{request.offset_x + actual_width:.1f} · "
            f"Y {request.offset_y:.1f}-{request.offset_y + actual_height:.1f}\n"
            f"{threshold_summary} · {request.laser_mode} · "
            f"S{request.min_power}-S{request.mark_power} · "
            f"扫描质量 {request.scan_quality:.1f} 线/mm · {scan_line_count} 条轨迹 · "
            f"{request.scan_direction_label}{vector_stats} · {len(data)} bytes"
        )
        return _ConversionResult(
            f"{source_name}.{suffix}.generated.gcode",
            data,
            preview,
            status,
        )

    def _convert_image(self) -> None:
        if self._source_image.isNull():
            self.image_status.setText("请先生成或导入图像")
            return
        mode = self.extract_mode.currentData()
        transform_mode = (
            Qt.FastTransformation
            if self.resize_mode_combo.currentData() == "nearest"
            else Qt.SmoothTransformation
        )
        if mode in RASTER_EXTRACTION_MODES:
            image = self._scale_for_scan_quality(
                self._source_image,
                transform_mode,
            )
        else:
            image = self._scale_for_vector_quality(
                self._source_image,
                transform_mode,
            )
        if (
            self.downsample_check.isChecked()
            and mode in ("vector", "lasergrbl_vector")
            and self.downsample_factor.value() > 1.0
        ):
            factor = self.downsample_factor.value()
            image = image.scaled(
                max(1, int(image.width() / factor)),
                max(1, int(image.height() / factor)),
                Qt.KeepAspectRatio,
                Qt.FastTransformation,
            )
        rows = self._grayscale_rows_from_image(image)
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
        min_power = min(self.power_min_spin.value(), self.power_spin.value())
        mark_power = max(self.power_min_spin.value(), self.power_spin.value())
        min_area = float(self.vector_min_area.value()) if self.spot_removal_check.isChecked() else 1.0
        smoothing = self.vector_simplify.value() if self.smoothing_check.isChecked() else 0.0
        path_count = 0
        point_count = 0
        auto_binary = False
        fill_mask: list[list[bool]] | None = None

        def merge_jobs(first: str, second: str) -> str:
            first_lines = first.strip().splitlines()
            second_lines = second.strip().splitlines()
            if first_lines and first_lines[-1] == "M5":
                first_lines.pop()
            if second_lines and second_lines[0].startswith(("M3 ", "M4 ")):
                second_lines = second_lines[1:]
            return "\n".join(first_lines + second_lines) + "\n"

        def binary_job(
            mask: list[list[bool]],
            *,
            direction: str,
            speed: int,
        ) -> str:
            binary_rows = [
                [0 if is_dark else 255 for is_dark in row]
                for row in mask
            ]
            return grayscale_rows_to_gcode(
                binary_rows,
                target_width_mm,
                speed,
                height_mm=target_height_mm,
                power_min_s=0,
                power_max_s=mark_power,
                laser_mode=laser_mode,
                x_offset_mm=offset_x,
                y_offset_mm=offset_y,
                direction=direction,
                unidirectional=self.unidirectional_scan_check.isChecked(),
                rapid_white=self.rapid_white_check.isChecked(),
            )

        def fill_job(mask: list[list[bool]], fill_mode: str) -> str:
            if fill_mode == "cross":
                horizontal = binary_job(
                    mask,
                    direction="horizontal",
                    speed=self.fill_speed_spin.value(),
                )
                vertical = binary_job(
                    mask,
                    direction="vertical",
                    speed=self.fill_speed_spin.value(),
                )
                return merge_jobs(horizontal, vertical)
            direction = {
                "vertical": "vertical",
                "diagonal": "diagonal",
            }.get(fill_mode, "horizontal")
            return binary_job(
                mask,
                direction=direction,
                speed=self.fill_speed_spin.value(),
            )

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

        actual_width, actual_height = self._fitted_mark_size_mm(self._source_image)
        if (
            offset_x + actual_width > 99.0005
            or offset_y + actual_height > 99.0005
        ):
            self.image_status.setText(
                "目标图形超出 99×99 mm 工作区："
                f"X 至 {offset_x + actual_width:.1f} mm，"
                f"Y 至 {offset_y + actual_height:.1f} mm"
            )
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
                        curve_smoothing=smoothing,
                        adaptive_quality=self.adaptive_quality_check.isChecked(),
                    ),
                )
                contours = result.contours
                mask = result.mask
                preview_rows = result.rows
                mode_label = "LaserGRBL风格矢量"
                algorithm = (
                    f"独立轮廓提取（非 Potrace） · 亮度 {self.lasergrbl_brightness_spin.value()} · "
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
                rapid_travel=self.rapid_white_check.isChecked(),
            )
            selected_fill = str(self.vector_fill_combo.currentData())
            if selected_fill != "none":
                fill_image = self._scale_for_scan_quality(
                    self._source_image,
                    transform_mode,
                    quality_lines_mm=self.fill_quality_spin.value(),
                )
                fill_rows = self._grayscale_rows_from_image(fill_image)
                fill_tone_rows = apply_lasergrbl_tone(
                    fill_rows,
                    brightness=self.lasergrbl_brightness_spin.value(),
                    contrast=self.lasergrbl_contrast_spin.value(),
                    white_clip=self.lasergrbl_white_clip_spin.value(),
                    black_clip=self.lasergrbl_black_clip_spin.value(),
                )
                fill_mask = prepare_laser_mask(
                    fill_tone_rows,
                    self.threshold_spin.value(),
                    adaptive=mode != "lasergrbl_vector",
                    edge_enhance=mode == "vector",
                    min_area_px=min_area,
                )
                fill_text = fill_job(fill_mask, selected_fill)
                text = merge_jobs(fill_text, text)
                algorithm += f" · {self.vector_fill_combo.currentText()}"
            self._converted_image = (
                self._build_vector_fill_preview(
                    fill_mask,
                    contours,
                    image.width(),
                    image.height(),
                )
                if fill_mask is not None
                else self._build_vector_preview(preview_rows, contours)
            )
            path_count = len(contours)
            point_count = sum(max(0, len(contour) - 1) for contour in contours)
        elif mode == "dither":
            mask = dither_rows_to_mask(
                tone_rows,
                self.threshold_spin.value(),
                str(self.dither_algorithm_combo.currentData()),
            )
            text = binary_job(
                mask,
                direction=str(self.scan_direction_combo.currentData()),
                speed=self.fill_speed_spin.value(),
            )
            self._converted_image = self._build_mask_preview(mask)
            algorithm = f"{self.dither_algorithm_combo.currentText()} 1bit"
        elif mode == "centerline":
            base_mask = prepare_laser_mask(
                tone_rows,
                self.threshold_spin.value(),
                adaptive=self.adaptive_quality_check.isChecked(),
                edge_enhance=False,
                min_area_px=min_area,
            )
            mask = centerline_mask(base_mask)
            center_paths = trace_centerline_paths(mask)
            text = centerline_paths_to_gcode(
                center_paths,
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
                rapid_travel=self.rapid_white_check.isChecked(),
            )
            self._converted_image = self._build_mask_preview(mask)
            path_count = len(center_paths)
            point_count = sum(len(path) for path in center_paths)
            algorithm = "Zhang–Suen 细化 · 8 连通中心线路径"
        else:
            auto_binary = (
                self.auto_line_art_check.isChecked()
                and self._looks_like_binary_line_art(tone_rows)
            )
            use_binary = (
                self.threshold_enabled_check.isChecked()
                or not self.raster_pwm_check.isChecked()
                or auto_binary
            )
            if use_binary:
                mask = prepare_laser_mask(
                    tone_rows,
                    self.threshold_spin.value(),
                    adaptive=False,
                    edge_enhance=False,
                    min_area_px=0,
                )
                text = binary_job(
                    mask,
                    direction=str(self.scan_direction_combo.currentData()),
                    speed=self.fill_speed_spin.value(),
                )
                self._converted_image = self._build_mask_preview(mask)
                if auto_binary and not self.threshold_enabled_check.isChecked():
                    algorithm = "自动识别黑白线稿 · 二值色段精简"
                else:
                    algorithm = "直通二值阈值" if mode == "passthrough" else "线到线 · 二值阈值"
            else:
                text = grayscale_rows_to_gcode(
                    tone_rows,
                    target_width_mm,
                    self.fill_speed_spin.value(),
                    height_mm=target_height_mm,
                    power_min_s=min_power,
                    power_max_s=mark_power,
                    laser_mode=laser_mode,
                    x_offset_mm=offset_x,
                    y_offset_mm=offset_y,
                    direction=str(self.scan_direction_combo.currentData()),
                    unidirectional=self.unidirectional_scan_check.isChecked(),
                    rapid_white=self.rapid_white_check.isChecked(),
                )
                self._converted_image = self._build_grayscale_tone_preview(
                    self._limit_preview_rows(tone_rows),
                    power_min_s=min_power,
                    power_max_s=mark_power,
                )
                algorithm = (
                    "直通灰度 PWM"
                    if mode == "passthrough"
                    else "线到线 · 8-bit 灰度 PWM"
                )
        output_bytes = len(text.encode("utf-8"))
        vector_stats = (
            f" · 路径 {path_count} · 节点 {point_count}"
            if mode in ("vector", "lasergrbl_vector", "centerline")
            else ""
        )
        scan_direction = str(self.scan_direction_combo.currentData())
        scan_line_count = {
            "vertical": image.width(),
            "diagonal": image.width() + image.height() - 1,
        }.get(scan_direction, image.height())
        scan_stats = (
            f" · 扫描质量 {self.scan_quality_spin.value():.1f} 线/mm"
            f" · {scan_line_count} 条轨迹"
            f" · {self.scan_direction_combo.currentText()}"
            if mode in ("scanline", "dither", "passthrough")
            else (
                f" · 填充质量 {self.fill_quality_spin.value():.1f} 线/mm"
                if mode in ("vector", "lasergrbl_vector")
                and self.vector_fill_combo.currentData() != "none"
                else ""
            )
        )
        threshold_summary = (
            f"阈值 {self.threshold_spin.value()}"
            if mode in ("dither", "vector", "lasergrbl_vector", "centerline")
            or self.threshold_enabled_check.isChecked()
            or not self.raster_pwm_check.isChecked()
            or auto_binary
            else "灰度 PWM"
        )
        self._converted_status = (
            f"{mode_label} · {algorithm} · 图形 {actual_width:.1f}×{actual_height:.1f} mm · "
            f"X {offset_x:.1f}-{offset_x + actual_width:.1f} · "
            f"Y {offset_y:.1f}-{offset_y + actual_height:.1f}\n"
            f"{threshold_summary} · {laser_mode} · "
            f"S{min_power}-S{mark_power}"
            f"{scan_stats}{vector_stats} · {output_bytes} bytes"
        )
        source = Path(self._image_path).stem if self._image_path else "generated_image"
        self.set_content(f"{source}.{suffix}.generated.gcode", text.encode("utf-8"))
        self.btn_preview_converted.setEnabled(True)
        self._set_preview_mode("converted")

    def build_outline_scan_payload(self) -> tuple[str, bytes] | None:
        source = self.get_gcode_bytes().decode("utf-8", errors="replace")
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
    def _limit_preview_rows(
        rows: list[list[int]] | list[bytearray],
        max_px: int = 1024,
    ) -> list[bytearray]:
        """Downsample a final-effect preview without changing G-code quality."""

        height = len(rows)
        width = len(rows[0])
        longest = max(width, height)
        if longest <= max_px:
            return [bytearray(row) for row in rows]
        scale = max_px / longest
        output_width = max(1, round(width * scale))
        output_height = max(1, round(height * scale))
        x_indices = [min(width - 1, int(x / scale)) for x in range(output_width)]
        return [
            bytearray(int(rows[min(height - 1, int(y / scale))][x]) for x in x_indices)
            for y in range(output_height)
        ]

    @staticmethod
    def _looks_like_binary_line_art(
        rows: list[list[int]] | list[bytearray],
    ) -> bool:
        """Detect high-contrast line art whose tiny gray noise explodes PWM."""

        height = len(rows)
        width = len(rows[0])
        stride = max(1, round((width * height / 200_000) ** 0.5))
        total = middle = dark = light = 0
        for row in rows[::stride]:
            for value in row[::stride]:
                gray = int(value)
                total += 1
                if 32 < gray < 223:
                    middle += 1
                if gray <= 32:
                    dark += 1
                elif gray >= 223:
                    light += 1
        if total < 256:
            return False
        return (
            middle / total <= 0.035
            and dark / total >= 0.002
            and light / total >= 0.20
        )

    @staticmethod
    def _limit_mask_preview(
        mask: list[list[bool]],
        max_px: int = 1024,
    ) -> list[list[bool]]:
        height = len(mask)
        width = len(mask[0])
        longest = max(width, height)
        if longest <= max_px:
            return mask
        scale = max_px / longest
        output_width = max(1, round(width * scale))
        output_height = max(1, round(height * scale))
        x_indices = [min(width - 1, int(x / scale)) for x in range(output_width)]
        return [
            [bool(mask[min(height - 1, int(y / scale))][x]) for x in x_indices]
            for y in range(output_height)
        ]

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
    def _build_grayscale_preview(
        power_rows: list[list[int]],
        *,
        max_power: int,
    ) -> QImage:
        height = len(power_rows)
        width = len(power_rows[0])
        effect = QImage(width, height, QImage.Format.Format_RGB32)
        ceiling = max(1, int(max_power))
        for y, row in enumerate(power_rows):
            for x, power in enumerate(row):
                shade = 255 - max(
                    0,
                    min(255, round(int(power) * 255.0 / ceiling)),
                )
                effect.setPixelColor(x, y, QColor(shade, shade, shade))
        return effect

    @staticmethod
    def _build_grayscale_tone_preview(
        rows: list[list[int]],
        *,
        power_min_s: int,
        power_max_s: int,
    ) -> QImage:
        """Render PWM preview through a 256-entry LUT without a power matrix."""

        height = len(rows)
        width = len(rows[0])
        ceiling = max(1, int(power_max_s))
        powers = grayscale_power_rows(
            [bytearray(range(256))],
            power_min_s=power_min_s,
            power_max_s=power_max_s,
        )[0]
        shades = bytearray(
            255
            - max(0, min(255, round(int(power) * 255.0 / ceiling)))
            for power in powers
        )
        effect = QImage(width, height, QImage.Format.Format_RGBA8888)
        data = memoryview(effect.bits()).cast("B")
        stride = effect.bytesPerLine()
        for y, row in enumerate(rows):
            output = bytearray(width * 4)
            for x, value in enumerate(row):
                shade = shades[int(value)]
                index = x * 4
                output[index] = shade
                output[index + 1] = shade
                output[index + 2] = shade
                output[index + 3] = 255
            start = y * stride
            data[start : start + width * 4] = output
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
                painter.drawLine(
                    QLineF(
                        float(first[0]),
                        float(first[1]),
                        float(second[0]),
                        float(second[1]),
                    )
                )
        painter.end()
        return effect

    @staticmethod
    def _build_vector_fill_preview(
        fill_mask: list[list[bool]],
        contours: list[Contour],
        contour_width: int,
        contour_height: int,
    ) -> QImage:
        """Render vector fill and outline in the fill raster's coordinate space."""
        fill_height = len(fill_mask)
        fill_width = len(fill_mask[0])
        effect = QImage(
            fill_width + 1,
            fill_height + 1,
            QImage.Format.Format_RGB32,
        )
        effect.fill(QColor("white"))
        fill_color = QColor("#cbd5e1")
        for y, row in enumerate(fill_mask):
            for x, is_dark in enumerate(row):
                if is_dark:
                    effect.setPixelColor(x, y, fill_color)

        scale_x = fill_width / max(1, int(contour_width))
        scale_y = fill_height / max(1, int(contour_height))
        painter = QPainter(effect)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setPen(QPen(QColor("#ef4444"), 1))
        for contour in contours:
            for first, second in zip(contour, contour[1:]):
                painter.drawLine(
                    QLineF(
                        float(first[0]) * scale_x,
                        float(first[1]) * scale_y,
                        float(second[0]) * scale_x,
                        float(second[1]) * scale_y,
                    )
                )
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
        if not self._source_image.isNull() or not self._converted_image.isNull():
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
            self.set_content(path, Path(path).read_bytes(), render_preview=True)

    def _clear(self) -> None:
        if self._showing_gcode_preview:
            self._abandon_gcode_preview()
            self._converted_image = QImage()
            self._converted_status = ""
            self.btn_preview_original.setText("原图")
            self.btn_preview_original.setEnabled(False)
            self.btn_preview_converted.setText("转换效果")
            self.btn_preview_converted.setEnabled(False)
            self.preview_label.clear()
            self.preview_label.setText(
                "▧\n\n未加载图像预览\n\n"
                "请在左侧请求生成图像，或者从本地选择图片导入"
            )
            self.image_status.setText("")
        self._path = ""
        self._gcode_data = b""
        self._editor_has_full_content = True
        self.editor.setReadOnly(False)
        self.btn_adjust_gcode.setEnabled(False)
        self.btn_load_full_gcode.hide()
        self.editor.clear()
        self.lbl_file.setText("未加载文件")
        self.lbl_size.setText("")
        self.gcode_loaded.emit("", b"")

    def _update_size(
        self,
        exact_size: int | None = None,
        *,
        highlight: bool | None = None,
        preview_only: bool = False,
    ) -> None:
        # Generated and normalized G-code is ASCII, so document character
        # count is an O(1) size estimate during manual edits.  set_content
        # supplies the exact byte count without serializing the whole editor a
        # second time.
        size = (
            max(0, self.editor.document().characterCount() - 1)
            if exact_size is None
            else max(0, int(exact_size))
        )
        if preview_only and size:
            suffix = " · 大文件仅显示开头预览，传输使用完整内容"
        else:
            suffix = " · 大文件语法高亮已关闭" if highlight is False and size else ""
        self.lbl_size.setText(f"{size} 字节{suffix}" if size else "")


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
            match_iterator = pattern.globalMatch(text)
            while match_iterator.hasNext():
                match = match_iterator.next()
                self.setFormat(match.capturedStart(), match.capturedLength(), fmt)
