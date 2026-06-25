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

    def test_rejects_coordinates_above_99mm(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "0-99.0mm"):
            prepare_gcode_for_upload("G90\nG1 X99.1 Y1\nM5\n")


if __name__ == "__main__":
    unittest.main()
