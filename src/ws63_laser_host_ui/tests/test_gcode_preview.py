from __future__ import annotations

import pytest

from app.gcode_preview import analyze_gcode_preview, render_gcode_preview


def test_preview_analysis_uses_only_laser_on_g1_segments() -> None:
    analysis = analyze_gcode_preview(
        """G21
G90
M4 S0
G0 X10 Y20
S250
G1 X30 Y20
G1 X30 Y40 S500
S0
G1 X90 Y90
M5
G1 X99 Y99 S1000
"""
    )

    assert analysis.segment_count == 2
    assert analysis.max_power_s == pytest.approx(500.0)
    assert analysis.geometry is not None
    assert analysis.geometry.min_x == pytest.approx(10.0)
    assert analysis.geometry.min_y == pytest.approx(20.0)
    assert analysis.geometry.max_x == pytest.approx(30.0)
    assert analysis.geometry.max_y == pytest.approx(40.0)


def test_preview_analysis_supports_modal_relative_moves_units_and_comments() -> None:
    analysis = analyze_gcode_preview(
        """(inch job)
G20 G90
M3 S600
G0 X1 Y1 ; rapid anchor
G1 X2 Y1
G91
X0 Y1
M5
"""
    )

    assert analysis.segment_count == 2
    assert analysis.geometry is not None
    assert analysis.geometry.min_x == pytest.approx(25.4)
    assert analysis.geometry.max_x == pytest.approx(50.8)
    assert analysis.geometry.min_y == pytest.approx(25.4)
    assert analysis.geometry.max_y == pytest.approx(50.8)


def test_rendered_preview_maps_pwm_power_to_visible_grayscale() -> None:
    result = render_gcode_preview(
        """G90
M4 S0
G0 X0 Y0
G1 X10 Y0 S100
G0 X0 Y10 S0
G1 X10 Y10 S1000
M5
""",
        max_canvas_px=320,
    )

    assert not result.image.isNull()
    colors = [
        result.image.pixelColor(x, y).red()
        for y in range(result.image.height())
        for x in range(result.image.width())
        if result.image.pixelColor(x, y).red() < 255
    ]
    assert colors
    assert min(colors) == 0
    assert max(colors) > 200


def test_finished_job_preview_is_rotated_180_degrees_for_machine_orientation() -> None:
    result = render_gcode_preview(
        """G90
G0 X0 Y0
M4 S1000
G1 X0 Y10
G1 X3 Y10
M5
""",
        max_canvas_px=320,
    )

    # The asymmetric L must appear at the upper-left.  The previous mapping
    # placed the short arm at the lower-right, so this guards both axes.
    assert result.image.pixelColor(90, 20).red() < 255
    assert result.image.pixelColor(result.image.width() - 90, result.image.height() - 20).red() == 255


def test_empty_or_laser_off_job_returns_an_explanatory_blank_result() -> None:
    result = render_gcode_preview("G90\nG0 X10 Y10\nG1 X20 Y20 S500\nM5\n")

    assert result.geometry is None
    assert result.segment_count == 0
    assert not result.image.isNull()


def test_preview_analysis_is_cancellable() -> None:
    checks = 0

    def cancel() -> None:
        nonlocal checks
        checks += 1
        raise RuntimeError("cancelled")

    source = "M4 S500\n" + ("G1 X1 Y1\n" * 600)
    with pytest.raises(RuntimeError, match="cancelled"):
        analyze_gcode_preview(source, cancel_check=cancel)
    assert checks == 1
