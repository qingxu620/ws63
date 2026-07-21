"""Safe post-processing for already generated G-code jobs."""
from __future__ import annotations

from dataclasses import dataclass
import math
import re
from typing import Callable

from app.gcode_validator import normalize_gcode, validate_gcode


_WORD_RE = re.compile(
    r"^([A-Z])([+-]?(?:(?:\d+(?:\.\d*)?)|(?:\.\d+))(?:E[+-]?\d+)?)$"
)


@dataclass(frozen=True)
class GcodeGeometry:
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
class GcodeTransformStats:
    geometry: GcodeGeometry | None
    max_power_s: float
    max_feed_mm_min: float
    line_count: int
    has_absolute_coordinates: bool
    has_relative_coordinates: bool
    has_coordinate_reset: bool
    has_homing: bool


@dataclass(frozen=True)
class GcodeTransformOptions:
    target_max_power_s: float | None = None
    target_max_feed_mm_min: float | None = None
    scale_percent: float = 100.0
    offset_x_mm: float = 0.0
    offset_y_mm: float = 0.0


@dataclass(frozen=True)
class GcodeTransformResult:
    text: str
    before: GcodeTransformStats
    after: GcodeTransformStats


def _command_number(word: str, letter: str) -> int | None:
    match = _WORD_RE.fullmatch(word)
    if match is None or match.group(1) != letter:
        return None
    value = float(match.group(2))
    return int(value) if value.is_integer() else None


def _word_value(word: str, letter: str) -> float | None:
    match = _WORD_RE.fullmatch(word)
    if match is None or match.group(1) != letter:
        return None
    return float(match.group(2))


def _format_number(value: float, *, decimals: int = 3) -> str:
    if not math.isfinite(value):
        raise ValueError("G-code 调整结果包含无效数字")
    if abs(value) < 0.5 * (10 ** -decimals):
        value = 0.0
    rounded = round(value)
    if abs(value - rounded) < 0.5 * (10 ** -decimals):
        return str(int(rounded))
    return f"{value:.{decimals}f}".rstrip("0").rstrip(".")


def _analyze_normalized(
    normalized: str,
    cancel_check: Callable[[], None] | None = None,
) -> GcodeTransformStats:
    absolute_mode = True
    current_x = 0.0
    current_y = 0.0
    points: list[tuple[float, float]] = []
    max_power = 0.0
    max_feed = 0.0
    has_absolute = False
    has_relative = False
    has_reset = False
    has_homing = False
    line_count = 0

    for line_index, line in enumerate(normalized.splitlines(), start=1):
        if cancel_check is not None and line_index % 256 == 0:
            cancel_check()
        words = line.split()
        if not words:
            continue
        line_count += 1
        g_commands = [
            command
            for word in words
            if (command := _command_number(word, "G")) is not None
        ]
        if 90 in g_commands:
            absolute_mode = True
        if 91 in g_commands:
            absolute_mode = False
        if 92 in g_commands:
            has_reset = True
            current_x = 0.0
            current_y = 0.0
            continue
        if 28 in g_commands:
            has_homing = True
            current_x = 0.0
            current_y = 0.0
            points.append((current_x, current_y))
            continue

        x_value = next(
            (value for word in words if (value := _word_value(word, "X")) is not None),
            None,
        )
        y_value = next(
            (value for word in words if (value := _word_value(word, "Y")) is not None),
            None,
        )
        if x_value is not None or y_value is not None:
            if absolute_mode:
                has_absolute = True
                current_x = current_x if x_value is None else x_value
                current_y = current_y if y_value is None else y_value
            else:
                has_relative = True
                current_x += 0.0 if x_value is None else x_value
                current_y += 0.0 if y_value is None else y_value
            points.append((current_x, current_y))

        for word in words:
            power = _word_value(word, "S")
            if power is not None:
                max_power = max(max_power, power)
            feed = _word_value(word, "F")
            if feed is not None:
                max_feed = max(max_feed, feed)

    geometry = None
    if points:
        geometry = GcodeGeometry(
            min(point[0] for point in points),
            min(point[1] for point in points),
            max(point[0] for point in points),
            max(point[1] for point in points),
        )
    return GcodeTransformStats(
        geometry=geometry,
        max_power_s=max_power,
        max_feed_mm_min=max_feed,
        line_count=line_count,
        has_absolute_coordinates=has_absolute,
        has_relative_coordinates=has_relative,
        has_coordinate_reset=has_reset,
        has_homing=has_homing,
    )


def analyze_gcode(
    text: str,
    cancel_check: Callable[[], None] | None = None,
) -> GcodeTransformStats:
    """Normalize and summarize a G-code job for the adjustment dialog."""

    return _analyze_normalized(
        normalize_gcode(text, cancel_check=cancel_check),
        cancel_check=cancel_check,
    )


def transform_gcode(
    text: str,
    options: GcodeTransformOptions,
    cancel_check: Callable[[], None] | None = None,
) -> GcodeTransformResult:
    """Scale power/feed and apply a safe 2-D affine job transform.

    Positive S and F values are scaled relative to their current maxima so PWM
    shades and multi-speed ratios remain intact.  Absolute XY coordinates are
    scaled about the job's lower-left bound and translated by the requested
    offsets; relative XY moves are scaled without repeated translation.
    """

    normalized = normalize_gcode(text, cancel_check=cancel_check)
    before = _analyze_normalized(normalized, cancel_check=cancel_check)
    values = (
        options.scale_percent,
        options.offset_x_mm,
        options.offset_y_mm,
    )
    if any(not math.isfinite(float(value)) for value in values):
        raise ValueError("缩放或偏移参数不是有效数字")
    scale = float(options.scale_percent) / 100.0
    if scale <= 0.0:
        raise ValueError("等比缩放必须大于 0%")
    offset_x = float(options.offset_x_mm)
    offset_y = float(options.offset_y_mm)
    geometry_changed = (
        abs(scale - 1.0) > 1e-12
        or abs(offset_x) > 1e-12
        or abs(offset_y) > 1e-12
    )
    if geometry_changed and before.geometry is None:
        raise ValueError("当前 G-code 没有可调整的 X/Y 轨迹")
    if geometry_changed and before.has_coordinate_reset:
        raise ValueError("包含 G92 坐标重置的任务暂不支持几何缩放或偏移")
    if geometry_changed and before.has_homing:
        raise ValueError("包含 G28 回零命令的任务暂不支持几何缩放或偏移")
    if (
        (abs(offset_x) > 1e-12 or abs(offset_y) > 1e-12)
        and not before.has_absolute_coordinates
    ):
        raise ValueError("纯 G91 相对坐标任务无法安全应用整体偏移")

    target_power = options.target_max_power_s
    if target_power is not None:
        target_power = float(target_power)
        if not math.isfinite(target_power) or not 0.0 <= target_power <= 1000.0:
            raise ValueError("目标最大功率必须在 S0-S1000 范围内")
        if before.max_power_s <= 0.0 and target_power > 0.0:
            raise ValueError("当前任务没有正功率 S 值，无法按比例调整功率")
    target_feed = options.target_max_feed_mm_min
    if target_feed is not None:
        target_feed = float(target_feed)
        if not math.isfinite(target_feed) or target_feed <= 0.0:
            raise ValueError("目标最大速度必须大于 0 mm/min")
        if before.max_feed_mm_min <= 0.0:
            raise ValueError("当前任务没有 F 速度值，无法按比例调整速度")

    power_factor = (
        1.0
        if target_power is None or before.max_power_s <= 0.0
        else target_power / before.max_power_s
    )
    feed_factor = (
        1.0
        if target_feed is None or before.max_feed_mm_min <= 0.0
        else target_feed / before.max_feed_mm_min
    )
    anchor_x = before.geometry.min_x if before.geometry is not None else 0.0
    anchor_y = before.geometry.min_y if before.geometry is not None else 0.0
    absolute_mode = True
    current_x = 0.0
    current_y = 0.0
    output_lines: list[str] = []

    for line_index, line in enumerate(normalized.splitlines(), start=1):
        if cancel_check is not None and line_index % 256 == 0:
            cancel_check()
        words = line.split()
        g_commands = [
            command
            for word in words
            if (command := _command_number(word, "G")) is not None
        ]
        if 90 in g_commands:
            absolute_mode = True
        if 91 in g_commands:
            absolute_mode = False
        if 92 in g_commands:
            current_x = 0.0
            current_y = 0.0
        if 28 in g_commands:
            current_x = 0.0
            current_y = 0.0

        x_value = next(
            (value for word in words if (value := _word_value(word, "X")) is not None),
            None,
        )
        y_value = next(
            (value for word in words if (value := _word_value(word, "Y")) is not None),
            None,
        )
        has_coordinates = x_value is not None or y_value is not None
        transformed_x: float | None = None
        transformed_y: float | None = None
        if has_coordinates and 92 not in g_commands and 28 not in g_commands:
            if absolute_mode:
                current_x = current_x if x_value is None else x_value
                current_y = current_y if y_value is None else y_value
                if geometry_changed:
                    transformed_x = anchor_x + (current_x - anchor_x) * scale + offset_x
                    transformed_y = anchor_y + (current_y - anchor_y) * scale + offset_y
            else:
                current_x += 0.0 if x_value is None else x_value
                current_y += 0.0 if y_value is None else y_value
                if geometry_changed:
                    transformed_x = None if x_value is None else x_value * scale
                    transformed_y = None if y_value is None else y_value * scale

        rebuilt: list[str] = []
        for word in words:
            letter = word[0]
            if geometry_changed and has_coordinates and letter in ("X", "Y"):
                continue
            if letter == "S":
                value = _word_value(word, "S")
                if value is not None and target_power is not None:
                    scaled_power = max(0.0, min(1000.0, value * power_factor))
                    rebuilt.append(f"S{int(round(scaled_power))}")
                    continue
            if letter == "F":
                value = _word_value(word, "F")
                if value is not None and target_feed is not None:
                    rebuilt.append(f"F{_format_number(value * feed_factor)}")
                    continue
            rebuilt.append(word)

        if geometry_changed and has_coordinates and 92 not in g_commands and 28 not in g_commands:
            if transformed_x is not None:
                rebuilt.append(f"X{_format_number(transformed_x)}")
            if transformed_y is not None:
                rebuilt.append(f"Y{_format_number(transformed_y)}")
        output_lines.append(" ".join(rebuilt))

    transformed = "\n".join(output_lines) + "\n"
    errors = validate_gcode(transformed, cancel_check=cancel_check)
    if errors:
        shown = "\n".join(f"  - {error}" for error in errors[:8])
        remainder = len(errors) - 8
        if remainder > 0:
            shown += f"\n  - 另有 {remainder} 项错误"
        raise ValueError(f"调整后的 G-code 未通过安全校验：\n{shown}")
    return GcodeTransformResult(
        text=transformed,
        before=before,
        after=_analyze_normalized(transformed, cancel_check=cancel_check),
    )
