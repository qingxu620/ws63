from __future__ import annotations

import unittest
import re

from transports.sle_tx_transport import (
    DATA_ACK_TIMEOUT_MIN_S,
    GCODE_LINE_MAX_BYTES,
    JOB_COMMIT_TIMEOUT_MIN_S,
    JOB_DATA_CHUNK_SIZE,
    JOB_MAX_SIZE,
    SleJobSerialClient,
    TX_UART_RESYNC_BYTE,
    WaitResult,
    parse_rx_status,
    plan_safe_preroll,
    prepare_gcode_for_rx,
)


class StatusParsingTests(unittest.TestCase):
    def test_host_upload_limit_matches_demo_cache(self) -> None:
        self.assertEqual(JOB_MAX_SIZE, 65536)

    def test_prepare_gcode_removes_standalone_metric_declaration(self) -> None:
        prepared, removed = prepare_gcode_for_rx(b"G21 ; millimeters\nG90\nM5\n")

        self.assertEqual(prepared, b"G90\nM5\n")
        self.assertEqual(removed, 1)

    def test_prepare_gcode_rejects_inch_mode_and_long_lines(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "G20"):
            prepare_gcode_for_rx(b"G20\n")
        with self.assertRaisesRegex(RuntimeError, "行长度上限"):
            prepare_gcode_for_rx(b"X" * (GCODE_LINE_MAX_BYTES + 1))

    def test_upload_only_rejects_jobs_larger_than_firmware_cache(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)

        with self.assertRaisesRegex(RuntimeError, "仅上传任务上限"):
            client.upload_job(1, b"X" * (JOB_MAX_SIZE + 1), 1.0)

    def test_upload_execute_can_bypass_host_size_audit(self) -> None:
        class FakeClient(SleJobSerialClient):
            def write_bytes(self, data: bytes, label: str = "") -> None:
                return None

            def send_line(self, line: str) -> None:
                return None

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                if "@OK resync" in pattern:
                    return WaitResult("@OK resync rx=aborted", 1)
                if "@DATA_READY" in pattern:
                    raise RuntimeError("passed size audit")
                return WaitResult("", 1)

        client = FakeClient(lambda channel, message: None)
        with self.assertRaisesRegex(RuntimeError, "passed size audit"):
            client.upload_job(
                1,
                b"X" * (JOB_MAX_SIZE + 1),
                1.0,
                enforce_job_size_limit=False,
            )

    def test_upload_rejects_preroll_without_explicit_auto_start(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)

        with self.assertRaisesRegex(RuntimeError, "仅上传"):
            client.upload_job(1, b"M5\n", 1.0, preroll_bytes=4096)

    def test_upload_rejects_reserved_uart_resync_byte(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)

        with self.assertRaisesRegex(RuntimeError, "0x18"):
            client.upload_job(1, b"G1 X1\x18Y1\n", 1.0)

    def test_preroll_plan_uses_host_side_safe_line(self) -> None:
        gcode = (b"G1 X1\n" * 700) + b"S0\n" + (b"G1 X2\n" * 100)

        plan = plan_safe_preroll(gcode, 4096)

        self.assertEqual(plan.safe_line_end, 4203)
        self.assertNotEqual(plan.effective % JOB_DATA_CHUNK_SIZE, 0)
        self.assertEqual(plan.effective, 4203)
        self.assertEqual(plan.reason, "laser_off_line")

    def test_preroll_plan_falls_back_to_line_after_safe_window(self) -> None:
        gcode = b"G1 X1\n" * 1500

        plan = plan_safe_preroll(gcode, 4096)

        self.assertGreaterEqual(plan.safe_line_end, 8192)
        self.assertEqual(plan.effective, plan.safe_line_end)
        self.assertEqual(plan.reason, "fallback_line")

    def test_upload_splits_data_chunk_at_preroll_boundary(self) -> None:
        writes: list[bytes] = []
        commands: list[str] = []
        ack_offsets: list[int] = []
        payload = b"A" * (JOB_DATA_CHUNK_SIZE + 50)
        preroll = JOB_DATA_CHUNK_SIZE + 17

        class FakeClient(SleJobSerialClient):
            def write_bytes(self, data: bytes, label: str = "") -> None:
                writes.append(data)

            def send_line(self, line: str) -> None:
                commands.append(line)

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                if "@OK resync" in pattern:
                    return WaitResult("@OK resync rx=aborted", 1)
                if "@DATA_READY" in pattern:
                    return WaitResult("@DATA_READY job=1 size=264", 1)
                if "@ACK type=16" in pattern:
                    return WaitResult("@ACK type=16 seq=0 status=0", 1)
                if "@OK data_resume" in pattern:
                    return WaitResult("@OK data_resume", 1)
                if "@JOB_READY" in pattern:
                    return WaitResult("@JOB_READY job=1 size=264", 1)
                match = re.search(r"offset=(\d+)\b", pattern)
                if match:
                    off = int(match.group(1))
                    ack_offsets.append(off)
                    suffix = " preroll=1" if off == preroll else ""
                    return WaitResult(f"@ACK type=2 seq=0 status=0 offset={off}{suffix}", 1)
                raise AssertionError(pattern)

        client = FakeClient(lambda channel, message: None)
        client.upload_job(
            1,
            payload,
            2.0,
            preroll_bytes=preroll,
            start_on_preroll=True,
            enforce_job_size_limit=False,
        )

        self.assertEqual([len(item) for item in writes[1:]], [JOB_DATA_CHUNK_SIZE, 17, 33])
        self.assertEqual(ack_offsets, [JOB_DATA_CHUNK_SIZE, preroll, len(payload)])
        self.assertIn("@DATA_RESUME", commands)

    def test_upload_uses_ten_second_floor_for_data_ack_waits(self) -> None:
        waits: list[tuple[str, float]] = []

        class FakeClient(SleJobSerialClient):
            def send_line(self, line: str) -> None:
                return None

            def write_bytes(self, data: bytes, label: str = "") -> None:
                return None

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                waits.append((pattern, timeout))
                if "@OK resync" in pattern:
                    return WaitResult("@OK resync rx=aborted", 1)
                if "offset=3" in pattern:
                    return WaitResult("@ACK type=2 seq=2 status=0 job=1 offset=3 credit=1", 1)
                if "@JOB_READY" in pattern:
                    return WaitResult("@JOB_READY job=1 size=3", 1)
                return WaitResult("@DATA_READY job=1 size=3", 1)

        client = FakeClient(lambda channel, message: None)
        client.upload_job(1, b"M5\n", 8.0)

        self.assertEqual(waits[1], ("@DATA_READY job=1", 8.0))
        self.assertEqual(waits[2][1], DATA_ACK_TIMEOUT_MIN_S)
        self.assertEqual(waits[3], ("@JOB_READY job=1", JOB_COMMIT_TIMEOUT_MIN_S))

    def test_upload_resyncs_before_begin_and_after_failure(self) -> None:
        writes: list[bytes] = []
        commands: list[str] = []
        resync_count = 0

        class FakeClient(SleJobSerialClient):
            def write_bytes(self, data: bytes, label: str = "") -> None:
                writes.append(data)

            def send_line(self, line: str) -> None:
                commands.append(line)

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                nonlocal resync_count
                if "@OK resync" in pattern:
                    resync_count += 1
                    return WaitResult("@OK resync rx=aborted", 1)
                raise TimeoutError("DATA_READY missing")

        client = FakeClient(lambda channel, message: None)
        with self.assertRaisesRegex(TimeoutError, "DATA_READY"):
            client.upload_job(1, b"M5\n", 1.0)

        self.assertEqual(writes, [bytes((TX_UART_RESYNC_BYTE,))] * 2)
        self.assertEqual(commands, ["@BEGIN 1 3 c84a"])
        self.assertEqual(resync_count, 2)

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
