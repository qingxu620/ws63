"""
G-Code 生成模块。

目标：
1. 生成严格遵守当前 WS63 固件习惯的 G-Code；
2. 保留后续对 LaserGRBL 导入、直接串口下发两种路径的兼容；
3. 支持红光边框预览 G-Code。
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterable, List, Sequence, Tuple

from .image_processing import NormalizedPath, compute_bounding_box


class GCodeGenerationError(RuntimeError):
    """G-Code 生成失败时抛出的异常。"""


def _format_xy(x_mm: float, y_mm: float) -> str:
    return f"X{x_mm:.3f} Y{y_mm:.3f}"


def _scale_point(point: Tuple[float, float], width_mm: float, height_mm: float) -> Tuple[float, float]:
    return point[0] * width_mm, point[1] * height_mm


def _fit_contours_to_work_area(
    contours: Sequence[NormalizedPath],
    width_mm: float,
    height_mm: float,
    margin_ratio: float = 0.08,
) -> List[NormalizedPath]:
    """
    根据用户设定的物理尺寸再次做一次等比例适配。

    原因：
    - 图像处理阶段只知道“归一化画布”；
    - 真实雕刻区域可能是 50x50，也可能是 80x50；
    - 这里必须再按物理宽高比做一次 fit，防止图形被拉伸。
    """

    min_x, min_y, max_x, max_y = compute_bounding_box(contours)
    src_w = max(max_x - min_x, 1e-6)
    src_h = max(max_y - min_y, 1e-6)

    usable_w = max(width_mm * (1.0 - 2.0 * margin_ratio), 1e-6)
    usable_h = max(height_mm * (1.0 - 2.0 * margin_ratio), 1e-6)
    scale = min(usable_w / src_w, usable_h / src_h)

    dst_w = src_w * scale
    dst_h = src_h * scale
    offset_x = (width_mm - dst_w) * 0.5
    offset_y = (height_mm - dst_h) * 0.5

    fitted: List[NormalizedPath] = []
    for path in contours:
        placed: NormalizedPath = []
        for x, y in path:
            placed.append(((x - min_x) * scale + offset_x, (y - min_y) * scale + offset_y))
        fitted.append(placed)
    return fitted


def generate_gcode(
    contours: Sequence[NormalizedPath],
    width_mm: float,
    height_mm: float,
    feed_rate: int = 6000,
    power: int = 200,
) -> List[str]:
    """
    生成轮廓雕刻 G-Code。

    格式严格遵守当前约束：
    - 头部: $I, G90
    - 每个轮廓起点: M5 + G0
    - 雕刻过程: M3 Sxxx + G1
    - 尾部: M5 + G0 X0 Y0
    """

    if not contours:
        raise GCodeGenerationError("轮廓为空，无法生成 G-Code")
    if width_mm <= 0 or height_mm <= 0:
        raise GCodeGenerationError("雕刻尺寸必须大于 0")
    if feed_rate <= 0:
        raise GCodeGenerationError("进给速度必须大于 0")
    if power < 0:
        raise GCodeGenerationError("激光功率不能为负数")

    fitted_contours = _fit_contours_to_work_area(contours, width_mm, height_mm)

    lines: List[str] = [
        "$I",
        "G90",
        "M5",
    ]

    for path in fitted_contours:
        if len(path) < 2:
            continue

        start_x, start_y = path[0]
        lines.append("M5")
        lines.append(f"G0 {_format_xy(start_x, start_y)}")
        lines.append(f"M3 S{int(power)}")

        for index, point in enumerate(path):
            x_mm, y_mm = point
            if index == 0:
                # 第一段也按 G1 发出去，保证下位机统一走线性插补逻辑。
                lines.append(f"G1 {_format_xy(x_mm, y_mm)} F{int(feed_rate)}")
            else:
                lines.append(f"G1 {_format_xy(x_mm, y_mm)} F{int(feed_rate)}")

        # 轮廓闭合能避免开口线段带来的视觉缺口。
        lines.append(f"G1 {_format_xy(start_x, start_y)} F{int(feed_rate)}")
        lines.append("M5")

    lines.extend(
        [
            "M5",
            "G0 X0.000 Y0.000",
        ]
    )
    return lines


def generate_preview_gcode(
    contours: Sequence[NormalizedPath],
    width_mm: float,
    height_mm: float,
) -> List[str]:
    """
    生成边界框预览 G-Code。

    预览时不开激光，只走最大外接矩形，用于现场确认雕刻范围。
    """

    if not contours:
        raise GCodeGenerationError("轮廓为空，无法生成边界预览")

    fitted_contours = _fit_contours_to_work_area(contours, width_mm, height_mm)
    min_x, min_y, max_x, max_y = compute_bounding_box(fitted_contours)
    x0 = min_x
    y0 = min_y
    x1 = max_x
    y1 = max_y

    return [
        "$I",
        "G90",
        "M5",
        f"G0 {_format_xy(x0, y0)}",
        f"G0 {_format_xy(x1, y0)}",
        f"G0 {_format_xy(x1, y1)}",
        f"G0 {_format_xy(x0, y1)}",
        f"G0 {_format_xy(x0, y0)}",
        "M5",
        "G0 X0.000 Y0.000",
    ]


def save_gcode_file(lines: Iterable[str], output_path: str) -> str:
    output = Path(output_path).resolve()
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return str(output)
