from __future__ import annotations

import unittest
import re

from transports.sle_tx_transport import (
    DATA_ACK_TIMEOUT_MIN_S,
    GCODE_LINE_MAX_BYTES,
    JOB_COMMIT_TIMEOUT_MIN_S,
    JOB_DATA_CHUNK_SIZE,
    JOB_EXEC_PREROLL_MAX_BYTES,
    JOB_MAX_SIZE,
    PREROLL_CONTROL_TIMEOUT_MIN_S,
    SleJobSerialClient,
    TX_DATA_BUSY_RE,
    TX_UART_CONTROL_BYTE,
    TX_UART_RESYNC_BYTE,
    UploadInterrupted,
    WaitResult,
    clamp_stream_preroll_request,
    crc16_ccitt,
    is_noisy_sdk_log,
    is_noisy_runtime_log,
    parse_rx_status,
    plan_safe_preroll,
    prepare_gcode_for_rx,
    should_display_serial_line,
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

    def test_large_stream_preroll_request_leaves_cache_headroom(self) -> None:
        capped, changed = clamp_stream_preroll_request(JOB_MAX_SIZE + 1, JOB_MAX_SIZE)

        self.assertTrue(changed)
        self.assertEqual(capped, JOB_EXEC_PREROLL_MAX_BYTES)

    def test_small_stream_preroll_request_is_not_capped(self) -> None:
        requested = JOB_DATA_CHUNK_SIZE * 2

        capped, changed = clamp_stream_preroll_request(JOB_MAX_SIZE - 1, requested)

        self.assertFalse(changed)
        self.assertEqual(capped, requested)

    def test_upload_rejects_preroll_without_explicit_auto_start(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)

        with self.assertRaisesRegex(RuntimeError, "仅上传"):
            client.upload_job(1, b"M5\n", 1.0, preroll_bytes=4096)

    def test_upload_rejects_reserved_uart_resync_byte(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)

        with self.assertRaisesRegex(RuntimeError, "0x18"):
            client.upload_job(1, b"G1 X1\x18Y1\n", 1.0)

    def test_upload_rejects_reserved_uart_control_byte(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)

        with self.assertRaisesRegex(RuntimeError, "0x19"):
            client.upload_job(1, b"G1 X1\x19Y1\n", 1.0)

    def test_wait_for_can_ignore_stale_unknown_command(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)
        client._lines.put("@ERR unknown_command")
        client._lines.put("@OK resync rx=aborted")

        result = client.wait_for(
            "@OK resync rx=aborted", 1.0, ignore_unknown_command=True
        )

        self.assertEqual(result.line, "@OK resync rx=aborted")

    def test_noisy_sdk_log_filter_only_drops_sle_announce_spam(self) -> None:
        self.assertTrue(is_noisy_sdk_log("[ACore] sle stop announce in, adv_id:1"))
        self.assertTrue(is_noisy_sdk_log("[ACore] sle start announce in, adv_id:1"))
        self.assertTrue(is_noisy_sdk_log("[ACore] sle adv cbk in, event:4 status:0"))
        self.assertFalse(is_noisy_sdk_log("[ACore] sle adv cbk in, event:4 status:1"))
        self.assertFalse(is_noisy_sdk_log("[ACore] serious fault"))
        self.assertFalse(is_noisy_sdk_log("@STATUS state=1 status=0 job=1 rx=0 total=10 free=1 lines=0"))

    def test_runtime_log_filter_hides_only_redundant_status_noise(self) -> None:
        self.assertTrue(is_noisy_runtime_log("xo update temp:4,diff:0,xo:0x3083c"))
        self.assertTrue(is_noisy_runtime_log("APP|[SYS INFO] mem: used:1, free:2; log: drop/all[0/2], at_recv 0."))
        self.assertTrue(is_noisy_runtime_log("@ACK type=2 seq=0 status=0 offset=214"))

        self.assertFalse(is_noisy_runtime_log("@NACK type=2 seq=0 status=9 offset=214"))
        self.assertFalse(is_noisy_runtime_log("[TX_TO] type=0x02 seq=1 retry=0"))
        self.assertFalse(is_noisy_runtime_log("[RX_SEND_TIMING] type=0x80 send_ms=300"))
        self.assertFalse(is_noisy_runtime_log("[HOST_TIMING] chunk=1 off=214"))

        self.assertFalse(should_display_serial_line("@ACK type=2 seq=0 status=0 offset=214"))
        self.assertTrue(should_display_serial_line("@NACK type=2 seq=0 status=9 offset=214"))

    def test_preroll_plan_uses_nearby_line_boundary(self) -> None:
        gcode = (b"G1 X1\n" * 700) + b"S0\n" + (b"G1 X2\n" * 100)

        plan = plan_safe_preroll(gcode, 4096)

        self.assertEqual(plan.safe_line_end, 4098)
        self.assertNotEqual(plan.effective % JOB_DATA_CHUNK_SIZE, 0)
        self.assertEqual(plan.effective, 4098)
        self.assertEqual(plan.reason, "line_boundary")

    def test_preroll_plan_rejects_distant_boundary(self) -> None:
        gcode = b"G1 X1\n" * 1500
        gcode = gcode[:4096] + (b"X" * 200) + b"\n" + gcode[4096:]

        plan = plan_safe_preroll(gcode, 4096)

        self.assertEqual(plan.effective, 0)
        self.assertEqual(plan.reason, "line_too_long")

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

    def test_preroll_control_waits_use_timeout_floor(self) -> None:
        waits: list[tuple[str, float]] = []
        payload = b"A" * (JOB_DATA_CHUNK_SIZE + 50)
        preroll = JOB_DATA_CHUNK_SIZE + 17

        class FakeClient(SleJobSerialClient):
            def write_bytes(self, data: bytes, label: str = "") -> None:
                return None

            def send_line(self, line: str) -> None:
                return None

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                waits.append((pattern, timeout))
                if "@OK resync" in pattern:
                    return WaitResult("@OK resync rx=aborted", 1)
                if "@DATA_READY" in pattern:
                    return WaitResult(f"@DATA_READY job=1 size={len(payload)}", 1)
                if "@ACK type=16" in pattern:
                    return WaitResult("@ACK type=16 seq=0 status=0", 1)
                if "@OK data_resume" in pattern:
                    return WaitResult("@OK data_resume", 1)
                if "@JOB_READY" in pattern:
                    return WaitResult(f"@JOB_READY job=1 size={len(payload)}", 1)
                match = re.search(r"offset=(\d+)\b", pattern)
                if match:
                    off = int(match.group(1))
                    suffix = " preroll=1" if off == preroll else ""
                    return WaitResult(f"@ACK type=2 seq=0 status=0 offset={off}{suffix}", 1)
                raise AssertionError(pattern)

        client = FakeClient(lambda channel, message: None)
        client.upload_job(
            1,
            payload,
            8.0,
            preroll_bytes=preroll,
            start_on_preroll=True,
            enforce_job_size_limit=False,
        )

        exec_wait = next(timeout for pattern, timeout in waits if "@ACK type=16" in pattern)
        resume_wait = next(timeout for pattern, timeout in waits if "@OK data_resume" in pattern)
        self.assertEqual(exec_wait, PREROLL_CONTROL_TIMEOUT_MIN_S)
        self.assertEqual(resume_wait, PREROLL_CONTROL_TIMEOUT_MIN_S)

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

    def test_wait_for_extends_on_data_busy_keepalive(self) -> None:
        client = SleJobSerialClient(lambda channel, message: None)
        client._lines.put("@BUSY type=2 offset=600 ack_offset=300 outstanding=300 window=900 waited=1000")
        client._lines.put("@ACK type=2 seq=0 status=0 offset=600")

        result = client.wait_for(
            r"@ACK type=2 .*status=0 .*offset=600\b",
            0.01,
            regex=True,
            keepalive_pattern=TX_DATA_BUSY_RE,
            max_timeout=1.0,
        )

        self.assertIn("offset=600", result.line)

    def test_priority_control_is_sent_after_current_data_chunk(self) -> None:
        writes: list[bytes] = []
        commands: list[str] = []
        payload = b"A" * (JOB_DATA_CHUNK_SIZE * 2)

        class FakeClient(SleJobSerialClient):
            def write_bytes(self, data: bytes, label: str = "") -> None:
                writes.append(data)

            def send_line(self, line: str) -> None:
                commands.append(line)

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                if "@OK resync" in pattern:
                    return WaitResult("@OK resync rx=aborted", 1)
                if "@DATA_READY" in pattern:
                    return WaitResult(f"@DATA_READY job=1 size={len(payload)}", 1)
                if "@ACK type=4" in pattern:
                    return WaitResult("@ACK type=4 seq=0 status=0", 1)
                if f"offset={JOB_DATA_CHUNK_SIZE}" in pattern:
                    self.request_priority_control("@ABORT", "@ACK type=4", 2.0)
                    return WaitResult(
                        f"@ACK type=2 seq=2 status=0 job=1 offset={JOB_DATA_CHUNK_SIZE} credit=1",
                        1,
                    )
                raise AssertionError(pattern)

        client = FakeClient(lambda channel, message: None)
        with self.assertRaises(UploadInterrupted):
            client.upload_job(1, payload, 2.0)

        self.assertEqual(len(commands), 1)
        self.assertTrue(commands[0].startswith(f"@BEGIN 1 {len(payload)} "))
        self.assertEqual([len(item) for item in writes], [1, JOB_DATA_CHUNK_SIZE, 2])
        self.assertEqual(writes[-1], bytes((TX_UART_CONTROL_BYTE, ord("A"))))

    def test_abort_request_converts_lost_data_ack_to_upload_interrupted(self) -> None:
        writes: list[bytes] = []
        commands: list[str] = []
        resync_count = 0
        payload = b"A" * JOB_DATA_CHUNK_SIZE

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
                if "@DATA_READY" in pattern:
                    return WaitResult(f"@DATA_READY job=1 size={len(payload)}", 1)
                if "@ACK type=4" in pattern:
                    return WaitResult("@ACK type=4 seq=0 status=0", 1)
                if "@ACK type=2" in pattern:
                    self.request_priority_control("@ABORT", "@ACK type=4", 2.0)
                    self._pop_priority_control()
                    raise TimeoutError("等待 @ACK type=2 offset 超时")
                raise AssertionError(pattern)

        client = FakeClient(lambda channel, message: None)
        with self.assertRaises(UploadInterrupted):
            client.upload_job(1, payload, 2.0)

        self.assertEqual(resync_count, 2)
        self.assertEqual(commands, [f"@BEGIN 1 {len(payload)} {crc16_ccitt(payload):04x}"])
        self.assertEqual(writes, [bytes((TX_UART_RESYNC_BYTE,)), payload, bytes((TX_UART_RESYNC_BYTE,))])

    def test_upload_unknown_command_after_pause_points_to_old_tx_firmware(self) -> None:
        payload = b"A" * JOB_DATA_CHUNK_SIZE

        class FakeClient(SleJobSerialClient):
            def write_bytes(self, data: bytes, label: str = "") -> None:
                return None

            def send_line(self, line: str) -> None:
                return None

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                if "@OK resync" in pattern:
                    return WaitResult("@OK resync rx=aborted", 1)
                if "@DATA_READY" in pattern:
                    return WaitResult("@DATA_READY job=1 size=214", 1)
                if "@ACK type=2" in pattern:
                    raise RuntimeError("收到错误响应: @ERR unknown_command")
                raise AssertionError(pattern)

        client = FakeClient(lambda channel, message: None)
        with self.assertRaisesRegex(RuntimeError, "旧版.*data_mode"):
            client.upload_job(1, payload, 2.0)

    def test_pause_stops_upload_until_resume(self) -> None:
        writes: list[bytes] = []
        commands: list[str] = []
        payload = b"A" * (JOB_DATA_CHUNK_SIZE * 2)

        class FakeClient(SleJobSerialClient):
            def write_bytes(self, data: bytes, label: str = "") -> None:
                writes.append(data)

            def send_line(self, line: str) -> None:
                commands.append(line)

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                if "@OK resync" in pattern:
                    return WaitResult("@OK resync rx=aborted", 1)
                if "@DATA_READY" in pattern:
                    return WaitResult(f"@DATA_READY job=1 size={len(payload)}", 1)
                if "@ACK type=19" in pattern:
                    self.request_priority_control(
                        "@EXEC_RESUME", "@ACK type=17", 2.0, interrupt_upload=False
                    )
                    return WaitResult("@ACK type=19 seq=0 status=0", 1)
                if "@ACK type=17" in pattern:
                    return WaitResult("@ACK type=17 seq=0 status=0", 1)
                if f"offset={JOB_DATA_CHUNK_SIZE}" in pattern:
                    self.request_priority_control(
                        "@EXEC_STOP", "@ACK type=19", 2.0, interrupt_upload=False
                    )
                    return WaitResult(
                        f"@ACK type=2 seq=2 status=0 job=1 offset={JOB_DATA_CHUNK_SIZE} credit=1",
                        1,
                    )
                if f"offset={len(payload)}" in pattern:
                    return WaitResult(
                        f"@ACK type=2 seq=3 status=0 job=1 offset={len(payload)} credit=1",
                        1,
                    )
                if "@JOB_READY" in pattern:
                    return WaitResult(f"@JOB_READY job=1 size={len(payload)}", 1)
                raise AssertionError(pattern)

        client = FakeClient(lambda channel, message: None)
        client.upload_job(1, payload, 2.0)

        self.assertEqual(len(commands), 1)
        self.assertEqual([len(item) for item in writes], [1, JOB_DATA_CHUNK_SIZE, 2, 2, JOB_DATA_CHUNK_SIZE])
        self.assertEqual(writes[2], bytes((TX_UART_CONTROL_BYTE, ord("S"))))
        self.assertEqual(writes[3], bytes((TX_UART_CONTROL_BYTE, ord("R"))))

    def test_resume_can_replace_unsent_pause_control(self) -> None:
        writes: list[bytes] = []

        class FakeClient(SleJobSerialClient):
            def write_bytes(self, data: bytes, label: str = "") -> None:
                writes.append(data)

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                if "@ACK type=17" in pattern:
                    return WaitResult("@ACK type=17 seq=0 status=0", 1)
                raise AssertionError(pattern)

        client = FakeClient(lambda channel, message: None)
        self.assertTrue(client.request_priority_control("@EXEC_STOP", "@ACK type=19", 2.0, interrupt_upload=False))
        self.assertTrue(client.request_priority_control("@EXEC_RESUME", "@ACK type=17", 2.0, interrupt_upload=False))

        client._dispatch_priority_control_locked()

        self.assertEqual(writes, [bytes((TX_UART_CONTROL_BYTE, ord("R")))])

    def test_focus_off_replaces_unsent_resume_but_not_pause(self) -> None:
        writes: list[bytes] = []

        class FakeClient(SleJobSerialClient):
            def write_bytes(self, data: bytes, label: str = "") -> None:
                writes.append(data)

            def wait_for(self, pattern: str, timeout: float, **kwargs) -> WaitResult:
                if "@OK focus_off" in pattern:
                    return WaitResult("@OK focus_off", 1)
                if "@ACK type=19" in pattern:
                    return WaitResult("@ACK type=19 seq=0 status=0", 1)
                raise AssertionError(pattern)

        client = FakeClient(lambda channel, message: None)
        self.assertTrue(client.request_priority_control("@EXEC_RESUME", "@ACK type=17", 2.0, interrupt_upload=False))
        self.assertTrue(client.request_priority_control("@FOCUS_OFF", "@OK focus_off", 2.0, interrupt_upload=False))
        client._dispatch_priority_control_locked()
        self.assertEqual(writes[-1], bytes((TX_UART_CONTROL_BYTE, ord("F"))))

        client = FakeClient(lambda channel, message: None)
        self.assertTrue(client.request_priority_control("@EXEC_STOP", "@ACK type=19", 2.0, interrupt_upload=False))
        self.assertFalse(client.request_priority_control("@FOCUS_OFF", "@OK focus_off", 2.0, interrupt_upload=False))
        client._dispatch_priority_control_locked()
        self.assertEqual(writes[-1], bytes((TX_UART_CONTROL_BYTE, ord("S"))))

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
