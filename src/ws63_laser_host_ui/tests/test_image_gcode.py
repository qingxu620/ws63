from __future__ import annotations

import unittest
import re

from app.image_gcode import (
    LaserGrblVectorOptions,
    apply_lasergrbl_tone,
    lasergrbl_style_vectorize,
    prepare_laser_mask,
    raster_mask_to_gcode,
    raster_rows_to_gcode,
    simplify_vector_contours,
    trace_dark_runs,
    trace_dark_runs_from_mask,
    trace_vector_contours,
    trace_vector_contours_from_mask,
    vector_contours_to_gcode,
)


class ImageGcodeTests(unittest.TestCase):
    def test_trace_dark_runs_groups_adjacent_dark_pixels(self) -> None:
        rows = [[255, 0, 10, 255, 20, 255]]

        self.assertEqual(trace_dark_runs(rows, threshold=128), [[(1, 2), (4, 4)]])

    def test_prepare_laser_mask_recovers_mid_tone_subject(self) -> None:
        rows = [
            [240, 240, 240, 240, 240],
            [240, 180, 180, 180, 240],
            [240, 180, 180, 180, 240],
            [240, 180, 180, 180, 240],
            [240, 240, 240, 240, 240],
        ]

        mask = prepare_laser_mask(
            rows,
            threshold=128,
            adaptive=True,
            adaptive_radius=2,
            adaptive_bias=10,
        )

        self.assertTrue(mask[2][2])
        self.assertFalse(mask[0][0])
        self.assertEqual(trace_dark_runs_from_mask(mask)[2], [(1, 3)])

    def test_prepare_laser_mask_removes_small_noise_components(self) -> None:
        rows = [
            [255, 255, 255, 255],
            [255, 0, 255, 255],
            [255, 255, 0, 0],
            [255, 255, 0, 0],
        ]

        mask = prepare_laser_mask(
            rows,
            threshold=128,
            adaptive=False,
            min_area_px=2,
        )

        self.assertFalse(mask[1][1])
        self.assertTrue(mask[2][2])
        self.assertTrue(mask[3][3])

    def test_lasergrbl_tone_white_clip_cleans_light_background(self) -> None:
        rows = [[244, 230, 120, 60]]

        adjusted = apply_lasergrbl_tone(rows, brightness=0, contrast=0, white_clip=230)

        self.assertEqual(adjusted[0][0], 255)
        self.assertEqual(adjusted[0][1], 255)
        self.assertLess(adjusted[0][2], 230)

    def test_lasergrbl_style_vectorize_filters_spots_and_simplifies(self) -> None:
        rows = [
            [255, 255, 255, 255, 255, 255, 255, 255],
            [255, 0, 255, 255, 255, 255, 255, 255],
            [255, 255, 0, 0, 0, 0, 255, 255],
            [255, 255, 0, 0, 0, 0, 255, 255],
            [255, 255, 0, 0, 0, 0, 255, 255],
            [255, 255, 0, 0, 0, 0, 255, 255],
            [255, 255, 255, 255, 255, 255, 255, 255],
        ]

        result = lasergrbl_style_vectorize(
            rows,
            LaserGrblVectorOptions(
                threshold=128,
                white_clip=245,
                spot_remove_px=2,
                smoothing_px=0.5,
            ),
        )

        self.assertFalse(result.mask[1][1])
        self.assertTrue(result.contours)
        self.assertLessEqual(len(result.contours), len(result.raw_contours))

    def test_edge_enhance_marks_gray_transitions_for_vector_mode(self) -> None:
        rows = [
            [240, 240, 240, 240, 240],
            [240, 220, 160, 220, 240],
            [240, 220, 160, 220, 240],
            [240, 220, 160, 220, 240],
            [240, 240, 240, 240, 240],
        ]

        mask = prepare_laser_mask(
            rows,
            threshold=128,
            adaptive=False,
            edge_enhance=True,
            edge_threshold=40,
        )

        self.assertTrue(any(mask[y][1] or mask[y][2] or mask[y][3] for y in range(1, 4)))
        contours = trace_vector_contours_from_mask(mask, min_area_px=1)
        self.assertTrue(contours)

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

        self.assertEqual(gcode.splitlines()[0], "M4 S0")
        self.assertNotIn("G21", gcode.splitlines())
        self.assertNotIn("G90", gcode.splitlines())
        self.assertFalse(
            any(line.lstrip().startswith((";", "(")) for line in gcode.splitlines())
        )
        self.assertIn("F3000", gcode)
        self.assertIn("S1000", gcode)
        self.assertNotIn("M3", gcode)
        self.assertNotIn("M30", gcode)
        self.assertIn("G0 X0.000 Y25.000", gcode)
        self.assertIn("G1 X50.000 Y25.000", gcode)
        self.assertTrue(gcode.endswith("S0\nM5\n"))

    def test_raster_mask_to_gcode_emits_scaled_segments(self) -> None:
        mask = [
            [True, True, False, False],
            [False, True, True, False],
        ]

        gcode = raster_mask_to_gcode(
            mask,
            width_mm=100.0,
            speed_mm_min=3000,
        )

        self.assertIn("G0 X0.000 Y25.000", gcode)
        self.assertIn("G1 X50.000 Y25.000", gcode)
        self.assertIn("G0 X75.000 Y0.000", gcode)
        self.assertIn("G1 X25.000 Y0.000", gcode)
        self.assertTrue(gcode.endswith("S0\nM5\n"))

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

    def test_vector_gcode_converts_top_left_image_y_to_machine_y(self) -> None:
        gcode = vector_contours_to_gcode(
            [[(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)]],
            image_width=2,
            image_height=2,
            width_mm=20.0,
            speed_mm_min=3000,
            height_mm=20.0,
            optimize_paths=False,
        )

        self.assertIn("G0 X0.000 Y20.000", gcode)
        self.assertIn("G1 X10.000 Y20.000", gcode)
        self.assertIn("G1 X10.000 Y10.000", gcode)

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

        self.assertEqual(gcode.splitlines()[0], "M4 S0")
        self.assertIn("S1000", gcode)
        self.assertNotIn("G90", gcode.splitlines())
        self.assertNotIn("M3", gcode)
        self.assertNotIn("M30", gcode)
        self.assertTrue(coordinates)
        self.assertGreaterEqual(min(coordinates), 0.0)
        self.assertLessEqual(max(coordinates), 60.0)
        self.assertTrue(gcode.endswith("S0\nM5\n"))


if __name__ == "__main__":
    unittest.main()
