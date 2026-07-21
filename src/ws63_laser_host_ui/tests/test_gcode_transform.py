from __future__ import annotations

import pytest

from app.gcode_transform import (
    GcodeTransformOptions,
    analyze_gcode,
    transform_gcode,
)


def test_transform_scales_power_speed_geometry_and_offset() -> None:
    source = """G90
M4 S0
F1000
G0 X10 Y20
S250
G1 X30 Y40 S500
M5
"""

    result = transform_gcode(
        source,
        GcodeTransformOptions(
            target_max_power_s=800,
            target_max_feed_mm_min=2000,
            scale_percent=200,
            offset_x_mm=5,
            offset_y_mm=-5,
        ),
    )

    assert "F2000" in result.text
    assert "S400" in result.text
    assert "S800" in result.text
    assert "G0 X15 Y15" in result.text
    assert "G1 S800 X55 Y55" in result.text
    assert result.after.geometry is not None
    assert result.after.geometry.min_x == pytest.approx(15.0)
    assert result.after.geometry.max_y == pytest.approx(55.0)


def test_transform_preserves_pwm_and_multi_feed_ratios() -> None:
    source = "M4 S0\nF1000\nS100\nG1 X1 Y1 F500 S400\nM5\n"

    result = transform_gcode(
        source,
        GcodeTransformOptions(
            target_max_power_s=1000,
            target_max_feed_mm_min=3000,
        ),
    )

    assert "S250" in result.text
    assert "S1000" in result.text
    assert "F3000" in result.text
    assert "F1500" in result.text


def test_transform_scales_relative_moves_after_absolute_anchor() -> None:
    source = "G90\nG0 X10 Y10\nG91\nG1 X5 Y-2\nG1 X5 Y2\nM5\n"

    result = transform_gcode(
        source,
        GcodeTransformOptions(
            scale_percent=200,
            offset_x_mm=3,
            offset_y_mm=4,
        ),
    )

    assert "G0 X13 Y16" in result.text
    assert "G1 X10 Y-4" in result.text
    assert "G1 X10 Y4" in result.text
    assert result.after.geometry is not None
    assert result.after.geometry.min_x == pytest.approx(13.0)
    assert result.after.geometry.min_y == pytest.approx(12.0)
    assert result.after.geometry.max_x == pytest.approx(33.0)
    assert result.after.geometry.max_y == pytest.approx(16.0)


def test_transform_rejects_unsafe_relative_offset_and_coordinate_reset() -> None:
    with pytest.raises(ValueError, match="纯 G91"):
        transform_gcode(
            "G91\nG1 X5 Y5\nM5\n",
            GcodeTransformOptions(offset_x_mm=1),
        )

    with pytest.raises(ValueError, match="G92"):
        transform_gcode(
            "G90\nG92 X0 Y0\nG1 X5 Y5\nM5\n",
            GcodeTransformOptions(scale_percent=120),
        )


def test_transform_rejects_result_outside_99mm_workspace() -> None:
    with pytest.raises(ValueError, match="工作范围"):
        transform_gcode(
            "G90\nG0 X10 Y10\nG1 X60 Y20\nM5\n",
            GcodeTransformOptions(scale_percent=200),
        )


def test_analyze_reports_bounds_and_current_maxima() -> None:
    stats = analyze_gcode(
        "G90\nM4 S0\nF800\nG0 X2 Y3\nG1 X8 Y9 S600 F1200\nM5\n"
    )

    assert stats.geometry is not None
    assert stats.geometry.width == pytest.approx(6.0)
    assert stats.geometry.height == pytest.approx(6.0)
    assert stats.max_power_s == pytest.approx(600.0)
    assert stats.max_feed_mm_min == pytest.approx(1200.0)
