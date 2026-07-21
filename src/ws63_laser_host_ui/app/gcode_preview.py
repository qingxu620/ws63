"""Background-safe rendering of a finished G-code job into a raster preview."""
from __future__ import annotations

from dataclasses import dataclass
from io import StringIO
import math
import re
from typing import Callable

from PySide6.QtCore import QLineF
from PySide6.QtGui import QColor, QImage, QPainter, QPen


_WORD_RE = re.compile(
    r"([A-Z])\s*([+-]?(?:(?:\d+(?:\.\d*)?)|(?:\.\d+))(?:E[+-]?\d+)?)",
    re.IGNORECASE,
)
_PAREN_COMMENT_RE = re.compile(r"\([^)]*\)")


@dataclass(frozen=True)
class GcodePreviewGeometry:
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


@dataclass(frozen=True)
class GcodePreviewAnalysis:
    geometry: GcodePreviewGeometry | None
    segment_count: int
    max_power_s: float
    line_count: int


@dataclass(frozen=True)
class GcodePreviewResult:
    image: QImage
    geometry: GcodePreviewGeometry | None
    segment_count: int
    max_power_s: float
    line_count: int


SegmentConsumer = Callable[[float, float, float, float, float], None]


def _integer_code(value: float) -> int | None:
    if not math.isfinite(value) or not value.is_integer():
        return None
    return int(value)


def _scan_marking_segments(
    text: str,
    *,
    on_segment: SegmentConsumer | None = None,
    cancel_check: Callable[[], None] | None = None,
) -> GcodePreviewAnalysis:
    """Walk modal G-code and report only laser-on linear marking moves."""

    absolute_mode = True
    unit_scale = 1.0
    current_x = 0.0
    current_y = 0.0
    modal_motion: int | None = None
    laser_enabled = False
    current_power = 0.0
    line_count = 0
    segment_count = 0
    max_power = 0.0
    bounds: list[float] | None = None

    for raw_line_number, raw_line in enumerate(StringIO(text), start=1):
        if cancel_check is not None and raw_line_number % 256 == 0:
            cancel_check()
        line = _PAREN_COMMENT_RE.sub("", raw_line.split(";", 1)[0]).strip().upper()
        if not line:
            continue
        words = [
            (match.group(1).upper(), float(match.group(2)))
            for match in _WORD_RE.finditer(line)
        ]
        if not words:
            continue
        line_count += 1

        g_codes = [
            code
            for letter, value in words
            if letter == "G" and (code := _integer_code(value)) is not None
        ]
        m_codes = [
            code
            for letter, value in words
            if letter == "M" and (code := _integer_code(value)) is not None
        ]

        # Modal state on a block applies to the move in that same block.
        for code in g_codes:
            if code == 20:
                unit_scale = 25.4
            elif code == 21:
                unit_scale = 1.0
            elif code == 90:
                absolute_mode = True
            elif code == 91:
                absolute_mode = False
            elif code in (0, 1):
                modal_motion = code

        if any(code in (3, 4) for code in m_codes):
            laser_enabled = True
        if 5 in m_codes:
            laser_enabled = False

        s_value = next((value for letter, value in reversed(words) if letter == "S"), None)
        if s_value is not None and math.isfinite(s_value):
            current_power = max(0.0, s_value)

        x_word = next((value for letter, value in reversed(words) if letter == "X"), None)
        y_word = next((value for letter, value in reversed(words) if letter == "Y"), None)
        has_coordinates = x_word is not None or y_word is not None

        if 92 in g_codes:
            # G92 changes the logical position without creating a motion path.
            if x_word is not None:
                current_x = x_word * unit_scale
            if y_word is not None:
                current_y = y_word * unit_scale
            continue
        if 28 in g_codes:
            # Homing is a non-marking machine operation.
            current_x = 0.0
            current_y = 0.0
            continue
        if not has_coordinates or modal_motion not in (0, 1):
            continue

        next_x = current_x
        next_y = current_y
        if x_word is not None:
            x_value = x_word * unit_scale
            next_x = x_value if absolute_mode else current_x + x_value
        if y_word is not None:
            y_value = y_word * unit_scale
            next_y = y_value if absolute_mode else current_y + y_value

        if (
            modal_motion == 1
            and laser_enabled
            and current_power > 0.0
            and all(math.isfinite(value) for value in (current_x, current_y, next_x, next_y))
            and (abs(next_x - current_x) > 1e-12 or abs(next_y - current_y) > 1e-12)
        ):
            segment_count += 1
            max_power = max(max_power, current_power)
            if bounds is None:
                bounds = [
                    min(current_x, next_x),
                    min(current_y, next_y),
                    max(current_x, next_x),
                    max(current_y, next_y),
                ]
            else:
                bounds[0] = min(bounds[0], current_x, next_x)
                bounds[1] = min(bounds[1], current_y, next_y)
                bounds[2] = max(bounds[2], current_x, next_x)
                bounds[3] = max(bounds[3], current_y, next_y)
            if on_segment is not None:
                on_segment(current_x, current_y, next_x, next_y, current_power)

        current_x = next_x
        current_y = next_y

    geometry = None
    if bounds is not None:
        geometry = GcodePreviewGeometry(*bounds)
    return GcodePreviewAnalysis(
        geometry=geometry,
        segment_count=segment_count,
        max_power_s=max_power,
        line_count=line_count,
    )


def analyze_gcode_preview(
    text: str,
    *,
    cancel_check: Callable[[], None] | None = None,
) -> GcodePreviewAnalysis:
    """Analyze the visible marking portion of a finished G-code job."""

    return _scan_marking_segments(text, cancel_check=cancel_check)


def render_gcode_preview(
    text: str,
    *,
    max_canvas_px: int = 1200,
    cancel_check: Callable[[], None] | None = None,
) -> GcodePreviewResult:
    """Render laser-on G1 segments; power is represented by grayscale depth.

    The finished-job preview follows the operator-facing machine orientation:
    X increases to the right and Y increases upward.  Compared with the prior
    artwork reconstruction this is an explicit 180-degree display rotation;
    it does not modify the G-code or the coordinates sent to the machine.
    """

    analysis = analyze_gcode_preview(text, cancel_check=cancel_check)
    geometry = analysis.geometry
    if geometry is None or analysis.segment_count <= 0:
        image = QImage(640, 360, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        return GcodePreviewResult(
            image=image,
            geometry=None,
            segment_count=0,
            max_power_s=0.0,
            line_count=analysis.line_count,
        )

    longest = max(geometry.width, geometry.height, 1e-9)
    canvas_limit = max(256, min(2048, int(max_canvas_px)))
    padding = 20
    scale = (canvas_limit - 2 * padding) / longest
    content_width = max(1, int(math.ceil(geometry.width * scale)))
    content_height = max(1, int(math.ceil(geometry.height * scale)))
    image_width = max(160, content_width + 2 * padding)
    image_height = max(160, content_height + 2 * padding)
    left = (image_width - content_width) / 2.0
    top = (image_height - content_height) / 2.0

    image = QImage(image_width, image_height, QImage.Format.Format_RGB32)
    image.fill(QColor("white"))
    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)
    pens = [QPen(QColor(shade, shade, shade), 1.0) for shade in range(256)]
    power_ceiling = max(1e-9, analysis.max_power_s)
    draw_count = 0

    def draw_segment(x0: float, y0: float, x1: float, y1: float, power: float) -> None:
        nonlocal draw_count
        draw_count += 1
        if cancel_check is not None and draw_count % 1024 == 0:
            cancel_check()
        ratio = max(0.0, min(1.0, power / power_ceiling))
        shade = max(0, min(254, 255 - int(round(255.0 * ratio))))
        painter.setPen(pens[shade])
        painter.drawLine(
            QLineF(
                left + (x0 - geometry.min_x) * scale,
                top + (geometry.max_y - y0) * scale,
                left + (x1 - geometry.min_x) * scale,
                top + (geometry.max_y - y1) * scale,
            )
        )

    try:
        _scan_marking_segments(
            text,
            on_segment=draw_segment,
            cancel_check=cancel_check,
        )
    finally:
        painter.end()

    return GcodePreviewResult(
        image=image,
        geometry=geometry,
        segment_count=analysis.segment_count,
        max_power_s=analysis.max_power_s,
        line_count=analysis.line_count,
    )
