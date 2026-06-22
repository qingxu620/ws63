from __future__ import annotations

import unittest

from transports.sle_tx_transport import (
    DATA_ACK_TIMEOUT_MIN_S,
    GCODE_LINE_MAX_BYTES,
    JOB_MAX_SIZE,
    SleJobSerialClient,
    WaitResult,
    parse_rx_status,
    prepare_gcode_for_rx,
)


class StatusParsingTests(unittest.TestCase):
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

    def test_upload_uses_ten_second_floor_for_data_ack_waits(self) -> None:
        waits: list[tuple[str, float]] = []

        class FakeClient(SleJobSerialClient):
            def send_line(self, line: str) -> None:
                return None

            def write_bytes(self, data: bytes, label: str = "") -> None:
                return None

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                waits.append((pattern, timeout))
                if "offset=3" in pattern:
                    return WaitResult("@ACK type=2 seq=2 status=0 job=1 offset=3 credit=1", 1)
                return WaitResult("@DATA_READY job=1 size=3", 1)

        client = FakeClient(lambda channel, message: None)
        client.upload_job(1, b"M5\n", 8.0, wait_ready=False)

        self.assertEqual(waits[0], ("@DATA_READY job=1", 8.0))
        self.assertEqual(waits[1][1], DATA_ACK_TIMEOUT_MIN_S)

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


if __name__ == "__main__":
    unittest.main()
