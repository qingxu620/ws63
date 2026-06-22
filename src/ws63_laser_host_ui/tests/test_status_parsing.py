from __future__ import annotations

import unittest

from transports.sle_tx_transport import (
    GCODE_LINE_MAX_BYTES,
    JOB_MAX_SIZE,
    SleJobSerialClient,
    count_rx_job_lines,
    parse_rx_status,
    prepare_gcode_for_rx,
)


class StatusParsingTests(unittest.TestCase):
    def test_rx_job_line_count_matches_semicolon_and_whitespace_rules(self) -> None:
        data = b"\n; comment only\n G90 ; metric mode\nM5\n  G1 X1 Y1 ; move\n"

        self.assertEqual(count_rx_job_lines(data), 3)

    def test_prepare_gcode_removes_standalone_metric_declaration(self) -> None:
        prepared, removed = prepare_gcode_for_rx(b"G21 ; millimeters\nG90\nM5\n")

        self.assertEqual(prepared, b"G90\nM5\n")
        self.assertEqual(removed, 1)

    def test_prepare_gcode_rejects_inch_mode_and_long_lines(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "G20"):
            prepare_gcode_for_rx(b"G20\n")
        with self.assertRaisesRegex(RuntimeError, "行长度上限"):
            prepare_gcode_for_rx(b"X" * (GCODE_LINE_MAX_BYTES + 1))

    def test_upload_rejects_jobs_larger_than_firmware_cache(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)

        with self.assertRaisesRegex(RuntimeError, "任务上限"):
            client.upload_job(1, b"X" * (JOB_MAX_SIZE + 1), 1.0)

    def test_upload_rejects_preroll_without_explicit_auto_start(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)

        with self.assertRaisesRegex(RuntimeError, "仅上传"):
            client.upload_job(1, b"M5\n", 1.0, preroll_bytes=4096)

    def test_parses_firmware_status_format(self) -> None:
        status = parse_rx_status(
            "@STATUS state=3 status=0 job=7 rx=900 total=1200 free=300 lines=42"
        )

        self.assertIsNotNone(status)
        self.assertEqual(status.state, 3)
        self.assertEqual(status.job_id, 7)
        self.assertEqual(status.received_size, 900)
        self.assertEqual(status.job_total, 1200)
        self.assertEqual(status.cache_free, 300)
        self.assertEqual(status.executed_lines, 42)

    def test_parses_mockup_compatible_status_format(self) -> None:
        status = parse_rx_status(
            "@STATUS state=1 error=0 job_id=9 exec_line=124 "
            "cache_free=8192 cache_total=16384"
        )

        self.assertIsNotNone(status)
        self.assertEqual(status.state, 0)
        self.assertEqual(status.job_id, 9)
        self.assertEqual(status.received_size, 0)
        self.assertEqual(status.job_total, 16384)
        self.assertEqual(status.cache_free, 8192)
        self.assertEqual(status.executed_lines, 124)


if __name__ == "__main__":
    unittest.main()
