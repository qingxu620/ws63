from __future__ import annotations

import unittest

from app.gcode_validator import prepare_gcode_for_upload


class GcodeValidatorTests(unittest.TestCase):
    def test_rejects_reserved_uart_resync_byte(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "0x18"):
            prepare_gcode_for_upload("G90\nG1 X1\x18Y1\nM5\n")

    def test_accepts_99mm_work_area_edge(self) -> None:
        payload, _ = prepare_gcode_for_upload("G90\nG1 X99 Y99 S100\nM5\n")

        self.assertIn(b"X99 Y99", payload)

    def test_accepts_m4_dynamic_laser_mode(self) -> None:
        payload, _ = prepare_gcode_for_upload("M4 S500\nG1 X1 Y1 F1000\nM5\n")

        self.assertIn(b"M4 S500", payload)

    def test_normalizes_compact_words_before_upload(self) -> None:
        payload, _ = prepare_gcode_for_upload("G21G90\nM3S100\nG0X3.65Y17.1\n")

        self.assertEqual(payload, b"G90\nM3 S100\nG0 X3.65 Y17.1\n")

    def test_drops_common_noop_modal_commands(self) -> None:
        payload, _ = prepare_gcode_for_upload("G17 G21.0 G40 G49 G54 G64 G94 G90\nM5\n")

        self.assertEqual(payload, b"G90\nM5\n")

    def test_rejects_compact_unsupported_arc_commands(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "不支持的命令: G3"):
            prepare_gcode_for_upload("G3X1Y2\n")

    def test_rejects_coordinates_above_99mm(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "0-99.0mm"):
            prepare_gcode_for_upload("G90\nG1 X99.1 Y1\nM5\n")


if __name__ == "__main__":
    unittest.main()
