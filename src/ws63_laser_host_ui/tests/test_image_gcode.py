from __future__ import annotations

import unittest
import re

from app.image_gcode import (
    raster_rows_to_gcode,
    simplify_vector_contours,
    trace_dark_runs,
    trace_vector_contours,
    vector_contours_to_gcode,
)


class ImageGcodeTests(unittest.TestCase):
    def test_trace_dark_runs_groups_adjacent_dark_pixels(self) -> None:
        rows = [[255, 0, 10, 255, 20, 255]]

        self.assertEqual(trace_dark_runs(rows, threshold=128), [[(1, 2), (4, 4)]])

    def test_raster_rows_to_gcode_emits_scaled_segments(self) -> None:
        rows = [
            [0, 0, 255, 255],
            [255, 0, 0, 255],
        ]

        gcode = raster_rows_to_gcode(
            rows,
            threshold=128,
            width_mm=100.0,
            speed_mm_min=3000,
        )

        self.assertEqual(gcode.splitlines()[0], "G90")
        self.assertNotIn("G21", gcode.splitlines())
        self.assertFalse(
            any(line.lstrip().startswith((";", "(")) for line in gcode.splitlines())
        )
        self.assertIn("F3000", gcode)
        self.assertIn("M3 S0", gcode)
        self.assertIn("S1000", gcode)
        self.assertIn("G0 X0.000 Y0.000", gcode)
        self.assertIn("G1 X50.000 Y0.000", gcode)
        self.assertTrue(gcode.endswith("M5\nM30\n"))

    def test_raster_rows_to_gcode_rejects_ragged_rows(self) -> None:
        with self.assertRaisesRegex(ValueError, "same width"):
            raster_rows_to_gcode([[0, 255], [0]], 128, 10.0, 1000)

    def test_generated_gcode_uses_selected_mark_power(self) -> None:
        rows = [[0, 0, 255]]

        raster = raster_rows_to_gcode(
            rows,
            threshold=128,
            width_mm=30.0,
            speed_mm_min=3000,
            power_s=420,
        )
        vector = vector_contours_to_gcode(
            [[(0, 0), (1, 0), (1, 1), (0, 0)]],
            image_width=3,
            image_height=3,
            width_mm=30.0,
            speed_mm_min=3000,
            power_s=420,
        )

        self.assertIn("S420", raster)
        self.assertIn("S420", vector)
        self.assertNotIn("S1000", raster)
        self.assertNotIn("S1000", vector)

    def test_raster_rows_fit_inside_square_work_area(self) -> None:
        rows = [[0, 0] for _ in range(8)]

        gcode = raster_rows_to_gcode(
            rows,
            threshold=128,
            width_mm=60.0,
            speed_mm_min=3000,
            height_mm=60.0,
        )

        coordinates = [
            float(value)
            for value in re.findall(r"[XY](-?\d+(?:\.\d+)?)", gcode)
        ]
        self.assertTrue(coordinates)
        self.assertGreaterEqual(min(coordinates), 0.0)
        self.assertLessEqual(max(coordinates), 60.0)

    def test_vector_contours_trace_closed_outer_and_inner_boundaries(self) -> None:
        rows = [
            [0, 0, 0, 0, 0],
            [0, 255, 255, 255, 0],
            [0, 255, 0, 255, 0],
            [0, 255, 255, 255, 0],
            [0, 0, 0, 0, 0],
        ]

        contours = trace_vector_contours(rows, threshold=128, min_area_px=1)

        self.assertGreaterEqual(len(contours), 3)
        self.assertTrue(all(contour[0] == contour[-1] for contour in contours))

    def test_vector_simplification_reduces_stair_step_points(self) -> None:
        contour = [(0, 0), (1, 0), (2, 0), (2, 1), (2, 2), (1, 2), (0, 2), (0, 1), (0, 0)]

        simplified = simplify_vector_contours([contour], tolerance_px=0.2)

        self.assertEqual(len(simplified), 1)
        self.assertLess(len(simplified[0]), len(contour))
        self.assertEqual(simplified[0][0], simplified[0][-1])

    def test_vector_gcode_emits_closed_outline_inside_work_area(self) -> None:
        rows = [
            [255, 255, 255, 255],
            [255, 0, 0, 255],
            [255, 0, 0, 255],
            [255, 255, 255, 255],
        ]
        contours = simplify_vector_contours(
            trace_vector_contours(rows, threshold=128, min_area_px=1),
            tolerance_px=0.5,
        )

        gcode = vector_contours_to_gcode(contours, 4, 4, 60.0, 3000, 60.0)
        coordinates = [
            float(value)
            for value in re.findall(r"[XY](-?\d+(?:\.\d+)?)", gcode)
        ]

        self.assertEqual(gcode.splitlines()[0], "G90")
        self.assertIn("M3 S0", gcode)
        self.assertIn("S1000", gcode)
        self.assertTrue(coordinates)
        self.assertGreaterEqual(min(coordinates), 0.0)
        self.assertLessEqual(max(coordinates), 60.0)
        self.assertTrue(gcode.endswith("M5\nM30\n"))


if __name__ == "__main__":
    unittest.main()
