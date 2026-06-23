from __future__ import annotations

import unittest

from app.gcode_validator import prepare_gcode_for_upload


class GcodeValidatorTests(unittest.TestCase):
    def test_rejects_reserved_uart_resync_byte(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "0x18"):
            prepare_gcode_for_upload("G90\nG1 X1\x18Y1\nM5\n")


if __name__ == "__main__":
    unittest.main()
