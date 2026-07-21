from __future__ import annotations

import unittest
import re

from app.image_gcode import (
    DITHER_ALGORITHMS,
    LaserGrblVectorOptions,
    apply_lasergrbl_tone,
    centerline_mask,
    centerline_paths_to_gcode,
    dither_rows_to_mask,
    grayscale_power_rows,
    grayscale_rows_to_gcode,
    lasergrbl_style_vectorize,
    prepare_laser_mask,
    raster_mask_to_gcode,
    raster_rows_to_gcode,
    rgb_rows_to_grayscale,
    simplify_vector_contours,
    smooth_vector_contours,
    trace_centerline_paths,
    trace_dark_runs,
    trace_dark_runs_from_mask,
    trace_vector_contours,
    trace_vector_contours_from_mask,
    vector_contours_to_gcode,
)


class ImageGcodeTests(unittest.TestCase):
    def test_grayscale_formulas_and_alpha_match_import_expectations(self) -> None:
        pixels = [[
            (255, 0, 0, 255),
            (0, 255, 0, 255),
            (0, 0, 255, 255),
            (0, 0, 0, 0),
        ]]

        self.assertEqual(
            rgb_rows_to_grayscale(pixels, formula="simple_average")[0],
            [85, 85, 85, 255],
        )
        self.assertEqual(
            rgb_rows_to_grayscale(pixels, formula="weight_average")[0],
            [85, 113, 57, 255],
        )
        self.assertEqual(
            rgb_rows_to_grayscale(pixels, formula="optical_correct")[0],
            [76, 150, 29, 255],
        )
        self.assertEqual(
            rgb_rows_to_grayscale(
                pixels,
                formula="custom",
                red_weight=300,
                green_weight=0,
                blue_weight=0,
            )[0],
            [255, 0, 0, 255],
        )

    def test_grayscale_power_uses_full_s_range_and_only_pure_white_is_off(self) -> None:
        rows = [[0, 127, 254, 255]]

        powers = grayscale_power_rows(
            rows,
            power_min_s=100,
            power_max_s=900,
        )
        gcode = grayscale_rows_to_gcode(
            rows,
            width_mm=40,
            height_mm=10,
            speed_mm_min=1200,
            power_min_s=100,
            power_max_s=900,
        )

        self.assertEqual(powers, [[900, 502, 103, 0]])
        self.assertIn("S900", gcode)
        self.assertIn("S502", gcode)
        self.assertIn("S103", gcode)
        self.assertTrue(gcode.endswith("S0\nM5\n"))

    def test_all_lasergrbl_dither_choices_are_deterministic(self) -> None:
        rows = [
            [round((x + y) * 255 / 14) for x in range(8)]
            for y in range(8)
        ]

        outputs = {
            algorithm: dither_rows_to_mask(rows, 128, algorithm)
            for algorithm in DITHER_ALGORITHMS
        }

        self.assertEqual(set(outputs), set(DITHER_ALGORITHMS))
        for algorithm, mask in outputs.items():
            self.assertEqual((len(mask), len(mask[0])), (8, 8), algorithm)
            self.assertTrue(any(any(row) for row in mask), algorithm)
            self.assertEqual(
                mask,
                dither_rows_to_mask(rows, 128, algorithm),
                algorithm,
            )
        self.assertGreater(len({str(mask) for mask in outputs.values()}), 3)

    def test_random_dither_never_marks_pure_white_or_drops_pure_black(self) -> None:
        rows = [[255] * 10_000, [0] * 10_000]

        mask = dither_rows_to_mask(rows, 128, "random", random_seed=0)

        self.assertFalse(any(mask[0]))
        self.assertTrue(all(mask[1]))

    def test_component_filter_keeps_a_thin_diagonal_as_one_subject(self) -> None:
        rows = [
            [0 if x == y else 255 for x in range(10)]
            for y in range(10)
        ]

        mask = prepare_laser_mask(
            rows,
            threshold=128,
            adaptive=False,
            min_area_px=4,
        )

        self.assertEqual(sum(map(sum, mask)), 10)

    def test_centerline_is_traced_as_continuous_paths(self) -> None:
        solid_bar = [[True] * 11 for _ in range(5)]

        skeleton = centerline_mask(solid_bar)
        paths = trace_centerline_paths(skeleton)
        gcode = centerline_paths_to_gcode(
            paths,
            image_width=11,
            image_height=5,
            width_mm=22,
            height_mm=10,
            speed_mm_min=900,
            power_s=420,
        )

        self.assertGreater(sum(map(sum, skeleton)), 1)
        self.assertLess(sum(map(sum, skeleton)), 55)
        self.assertTrue(paths)
        self.assertIn("S420", gcode)
        self.assertIn("G1", gcode)
        self.assertLess(gcode.count("G0 "), sum(map(sum, skeleton)))

    def test_centerline_does_not_cut_diagonally_across_orthogonal_cross(self) -> None:
        cross = [
            [False, True, False],
            [True, True, True],
            [False, True, False],
        ]

        paths = trace_centerline_paths(cross)
        traced_edges = sum(max(0, len(path) - 1) for path in paths)

        self.assertEqual(traced_edges, 4)
        self.assertEqual(len(paths), 4)

    def test_centerline_keeps_an_isolated_pixel_as_a_short_mark(self) -> None:
        paths = trace_centerline_paths([[True]])
        gcode = centerline_paths_to_gcode(
            paths,
            image_width=1,
            image_height=1,
            width_mm=1,
            speed_mm_min=900,
        )

        self.assertEqual(paths, [[(0.0, 0.0)]])
        self.assertIn("S1000", gcode)
        self.assertIn("G1 X0.250 Y0.500", gcode)

    def test_centerline_keeps_a_two_by_two_component(self) -> None:
        skeleton = centerline_mask([[True, True], [True, True]])

        self.assertEqual(sum(map(sum, skeleton)), 1)

    def test_centerline_large_solid_area_converges_without_fixed_iteration_cap(self) -> None:
        skeleton = centerline_mask([[True] * 200 for _ in range(200)])

        self.assertGreaterEqual(sum(map(sum, skeleton)), 1)
        self.assertLessEqual(sum(map(sum, skeleton)), 4)

    def test_centerline_trace_collapses_straight_pixel_runs(self) -> None:
        paths = trace_centerline_paths([[True] * 1000])

        self.assertEqual(paths, [[(0.0, 0.0), (999.0, 0.0)]])

    def test_diagonal_scan_uses_one_consistent_pixel_axis(self) -> None:
        gcode = grayscale_rows_to_gcode(
            [[0, 0], [0, 0]],
            width_mm=2,
            height_mm=2,
            speed_mm_min=1000,
            direction="diagonal",
            unidirectional=True,
        )
        moves = [line for line in gcode.splitlines() if line.startswith(("G0 ", "G1 "))]

        self.assertIn("G0 X2.000 Y1.000", moves)
        self.assertTrue(any(line.startswith("G1 X1.000 Y0.000") for line in moves))
        self.assertIn("G0 X1.000 Y2.000", moves)
        self.assertTrue(any(line.startswith("G1 X0.000 Y1.000") for line in moves))

    def test_all_white_grayscale_job_has_no_motion(self) -> None:
        gcode = grayscale_rows_to_gcode(
            [[255] * 200 for _ in range(100)],
            width_mm=20,
            height_mm=10,
            speed_mm_min=1000,
        )

        self.assertFalse(any(line.startswith(("G0 ", "G1 ")) for line in gcode.splitlines()))
        self.assertTrue(gcode.endswith("S0\nM5\n"))

    def test_raster_can_disable_g0_fast_travel(self) -> None:
        gcode = grayscale_rows_to_gcode(
            [[0, 255, 0]],
            width_mm=3,
            height_mm=1,
            speed_mm_min=1000,
            rapid_white=False,
        )

        self.assertNotIn("G0 ", gcode)
        self.assertIn("G1 ", gcode)

    def test_gcode_builders_reject_nonfinite_or_out_of_image_geometry(self) -> None:
        with self.assertRaisesRegex(ValueError, "finite"):
            grayscale_rows_to_gcode(
                [[0]],
                width_mm=float("nan"),
                speed_mm_min=1000,
            )
        with self.assertRaisesRegex(ValueError, "inside the image"):
            centerline_paths_to_gcode(
                [[(2, 0)]],
                image_width=1,
                image_height=1,
                width_mm=1,
                speed_mm_min=1000,
            )

    def test_curve_smoothing_preserves_closed_vector_paths(self) -> None:
        contour = [(0, 0), (8, 0), (8, 8), (0, 8), (0, 0)]

        smoothed = smooth_vector_contours([contour], strength=1.0)[0]

        self.assertEqual(smoothed[0], smoothed[-1])
        self.assertGreater(len(smoothed), len(contour))
        self.assertTrue(any(not value.is_integer() for point in smoothed for value in point))

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
            [0, 255, 255, 255],
            [255, 255, 255, 255],
            [255, 255, 0, 0],
            [255, 255, 0, 0],
        ]

        mask = prepare_laser_mask(
            rows,
            threshold=128,
            adaptive=False,
            min_area_px=2,
        )

        self.assertFalse(mask[0][0])
        self.assertTrue(mask[2][2])
        self.assertTrue(mask[3][3])

    def test_lasergrbl_tone_white_clip_cleans_light_background(self) -> None:
        rows = [[244, 230, 120, 60]]

        adjusted = apply_lasergrbl_tone(
            rows,
            brightness=100,
            contrast=100,
            white_clip=26,
        )

        self.assertEqual(adjusted[0][0], 255)
        self.assertEqual(adjusted[0][1], 255)
        self.assertLess(adjusted[0][2], 230)

    def test_lasergrbl_tone_uses_linear_100_centered_controls(self) -> None:
        rows = [[10, 100, 200]]

        doubled = apply_lasergrbl_tone(
            rows,
            brightness=100,
            contrast=200,
            white_clip=0,
        )
        brighter = apply_lasergrbl_tone(
            rows,
            brightness=150,
            contrast=100,
            white_clip=0,
        )

        self.assertEqual(list(doubled[0]), [20, 200, 255])
        self.assertEqual(list(brighter[0]), [138, 228, 255])

    def test_lasergrbl_style_vectorize_filters_spots_and_simplifies(self) -> None:
        rows = [
            [0, 255, 255, 255, 255, 255, 255, 255],
            [255, 255, 255, 255, 255, 255, 255, 255],
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
                white_clip=5,
                spot_remove_px=2,
                smoothing_px=0.5,
            ),
        )

        self.assertFalse(result.mask[0][0])
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
        self.assertIn("G0 X100.000 Y0.000", gcode)
        self.assertIn("G1 X50.000 Y0.000", gcode)
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

        self.assertIn("G0 X100.000 Y0.000", gcode)
        self.assertIn("G1 X50.000 Y0.000", gcode)
        self.assertIn("G0 X25.000 Y25.000", gcode)
        self.assertIn("G1 X75.000 Y25.000", gcode)
        self.assertTrue(gcode.endswith("S0\nM5\n"))

    def test_vector_gcode_mirrors_image_x_into_machine_x(self) -> None:
        gcode = vector_contours_to_gcode(
            [[(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)]],
            image_width=4,
            image_height=2,
            width_mm=40.0,
            speed_mm_min=3000,
            height_mm=20.0,
            optimize_paths=False,
        )

        self.assertIn("G0 X40.000 Y0.000", gcode)
        self.assertIn("G1 X30.000 Y0.000", gcode)
        self.assertNotIn("G0 X0.000 Y0.000", gcode)

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
