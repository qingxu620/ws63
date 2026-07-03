"""
SLE TX Transport — core protocol client for WS63 SLE Job link.

Ported from src/ws63_laser_sle_job_host/main.py (SleJobSerialClient + SerialLogMonitor).
Adapted for PySide6 QThread: on_log callbacks become Qt signals.
"""
from __future__ import annotations

import queue
import re
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None

# ---- Protocol constants (shared with firmware) ----
BAUD_DEFAULT = 115200
BAUD_LOG_DEFAULT = 115200
JOB_DATA_CHUNK_SIZE = 214
JOB_MAX_SIZE = 65536
GCODE_LINE_MAX_BYTES = 120  # RX SLE_JOB_GCODE_LINE_MAX=128, leave 8-byte safety margin
DATA_ACK_TIMEOUT_MIN_S = 10.0
JOB_COMMIT_TIMEOUT_MIN_S = 12.0
TX_RESYNC_TIMEOUT_MIN_S = 12.0
TX_UART_RESYNC_BYTE = 0x18
TX_UART_CONTROL_BYTE = 0x19
SERIAL_WRITE_BURST_SIZE = 128
SERIAL_WRITE_BURST_GAP_S = 0.001
HOST_TIMING_FIRST_CHUNKS = 8
HOST_TIMING_EVERY_CHUNKS = 32
HOST_TIMING_SLOW_CHUNK_MS = 250
JOB_PREROLL_BYTES = 4096
JOB_PREROLL_SAFE_MAX_BYTES = 8192
TX_PRIORITY_CONTROL_CODES = {
    "@EXEC_STOP": b"S",
    "@EXEC_RESUME": b"R",
    "@ABORT": b"A",
    "@FOCUS_OFF": b"F",
}
TX_PRIORITY_CONTROL_OPPOSITES = {
    "@EXEC_STOP": "@EXEC_RESUME",
    "@EXEC_RESUME": "@EXEC_STOP",
}

RX_STATUS_RE = re.compile(
    r"@STATUS state=(\d+) status=(\d+) job=(\d+) rx=(\d+) total=(\d+) free=(\d+) lines=(\d+)"
)
RX_STATUS_COMPAT_RE = re.compile(
    r"@STATUS state=(\d+) error=(\d+) job_id=(\d+) exec_line=(\d+) "
    r"cache_free=(\d+) cache_total=(\d+)"
)
RX_EXEC_DONE_RE = re.compile(
    r"\[JOB_EXEC\]\s+done\s+job=(\d+)\b"
)


@dataclass
class WaitResult:
    line: str
    elapsed_ms: int


@dataclass(frozen=True)
class PriorityControl:
    command: str
    expect: str
    timeout: float
    interrupt_upload: bool


class UploadInterrupted(RuntimeError):
    """Raised when a queued control command intentionally stops upload."""


@dataclass(frozen=True)
class RxStatus:
    state: int
    error: int
    job_id: int
    received_size: int
    job_total: int
    cache_free: int


@dataclass(frozen=True)
class PrerollPlan:
    requested: int
    effective: int
    safe_line_end: int
    reason: str


def parse_rx_status(line: str) -> Optional[RxStatus]:
    """Parse both the firmware status line and the HTML-prototype field names."""
    match = RX_STATUS_RE.search(line)
    if match:
        return RxStatus(
            state=int(match.group(1)),
            error=int(match.group(2)),
            job_id=int(match.group(3)),
            received_size=int(match.group(4)),
            job_total=int(match.group(5)),
            cache_free=int(match.group(6)),
        )
    match = RX_STATUS_COMPAT_RE.search(line)
    if match:
        return RxStatus(
            state=max(0, int(match.group(1)) - 1),
            error=int(match.group(2)),
            job_id=int(match.group(3)),
            received_size=0,
            job_total=int(match.group(6)),
            cache_free=int(match.group(5)),
        )
    return None


def crc16_ccitt(data: bytes, initial: int = 0xFFFF) -> int:
    crc = initial & 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


def normalize_gcode_text(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def _line_final_laser_state_is_off(line: bytes) -> bool:
    text = line.decode("utf-8", errors="ignore")
    code = text.split(";", 1)[0].split("(", 1)[0]
    saw_laser_state = False
    laser_off = False
    for token in code.replace("\t", " ").split():
        upper = token.upper()
        if upper == "M5":
            saw_laser_state = True
            laser_off = True
        elif upper.startswith("S") and len(upper) > 1:
            try:
                value = float(upper[1:])
            except ValueError:
                continue
            saw_laser_state = True
            laser_off = value <= 0.0
    return saw_laser_state and laser_off


def plan_safe_preroll(gcode: bytes, requested: int) -> PrerollPlan:
    """Pick a Host-side preroll threshold that lands on a complete G-code line."""
    total = len(gcode)
    if requested <= 0 or total <= requested:
        return PrerollPlan(requested=requested, effective=0, safe_line_end=0, reason="disabled")

    fallback_after = max(requested, JOB_PREROLL_SAFE_MAX_BYTES)
    line_start = 0
    fallback_line_end = 0
    for index, byte in enumerate(gcode):
        if byte != 0x0A:
            continue
        line_end = index + 1
        line = gcode[line_start:index].rstrip(b"\r")
        line_start = line_end
        if line_end < requested:
            continue
        if _line_final_laser_state_is_off(line):
            return PrerollPlan(
                requested=requested,
                effective=line_end if line_end < total else 0,
                safe_line_end=line_end,
                reason="laser_off_line" if line_end < total else "near_eof",
            )
        if fallback_line_end == 0 and line_end >= fallback_after:
            fallback_line_end = line_end

    if fallback_line_end > 0:
        return PrerollPlan(
            requested=requested,
            effective=fallback_line_end if fallback_line_end < total else 0,
            safe_line_end=fallback_line_end,
            reason="fallback_line" if fallback_line_end < total else "near_eof",
        )
    return PrerollPlan(requested=requested, effective=0, safe_line_end=0, reason="no_line_boundary")


def prepare_gcode_for_rx(data: bytes) -> tuple[bytes, int]:
    """Normalize common unit declarations and enforce RX line constraints."""
    text = normalize_gcode_text(data.decode("utf-8", errors="replace"))
    had_trailing_newline = text.endswith("\n")
    lines = text.split("\n")
    output: list[str] = []
    removed_g21 = 0

    for line_number, line in enumerate(lines, start=1):
        if line_number == len(lines) and line == "" and had_trailing_newline:
            continue
        code = line.split(";", 1)[0].split("(", 1)[0].strip().upper()
        if re.fullmatch(r"G21(?:\.0+)?", code):
            removed_g21 += 1
            continue
        if re.search(r"(?:^|\s)G20(?:\.0+)?(?:\s|$)", code):
            raise RuntimeError(f"第 {line_number} 行使用 RX 不支持的英寸单位 G20")
        if len(line.encode("utf-8")) > GCODE_LINE_MAX_BYTES:
            raise RuntimeError(
                f"第 {line_number} 行超过 RX 行长度上限: "
                f"{len(line.encode('utf-8'))}/{GCODE_LINE_MAX_BYTES} bytes"
            )
        output.append(line)

    prepared = "\n".join(output)
    if had_trailing_newline:
        prepared += "\n"
    return prepared.encode("utf-8"), removed_g21


def available_ports() -> list[str]:
    if serial is None:
        return []
    return [f"{p.device} - {p.description or 'Serial'}" for p in list_ports.comports()]


def port_device(display: str) -> str:
    return display.split(" - ", 1)[0].strip()


class SleJobSerialClient:
    """Full-duplex serial client for the SLE Job protocol."""

    def __init__(self, on_log: Callable[[str, str], None]) -> None:
        self._on_log = on_log
        self._serial: Optional[serial.Serial] = None
        self._reader: Optional[threading.Thread] = None
        self._running = threading.Event()
        self._lines: queue.Queue[str] = queue.Queue()
        self._write_lock = threading.Lock()
        self._command_lock = threading.Lock()
        self._priority_lock = threading.Lock()
        self._priority_control: Optional[PriorityControl] = None
        self._upload_paused = False

    def is_open(self) -> bool:
        return self._serial is not None and self._serial.is_open

    def open(self, port: str, baud: int) -> None:
        self.close()
        self._serial = serial.Serial(port, baudrate=baud, timeout=0.05, write_timeout=2.0)
        self._serial.reset_input_buffer()
        self._serial.reset_output_buffer()
        self._running.set()
        self._reader = threading.Thread(target=self._read_loop, name="sle-uart-reader", daemon=True)
        self._reader.start()
        self._on_log("status", f"已连接 {port} @ {baud}")

    def close(self) -> None:
        self._running.clear()
        if self._reader and self._reader.is_alive():
            self._reader.join(timeout=0.5)
        self._reader = None
        if self._serial:
            try:
                self._serial.close()
            except Exception:
                pass
        self._serial = None

    @property
    def port(self) -> Optional[str]:
        return self._serial.port if self._serial else None

    def _read_loop(self) -> None:
        assert self._serial is not None
        buf = ""
        while self._running.is_set() and self._serial and self._serial.is_open:
            try:
                chunk = self._serial.read(self._serial.in_waiting or 1)
            except Exception as exc:
                self._on_log("error", f"串口读取失败: {exc}")
                break
            if not chunk:
                continue
            buf += chunk.decode("utf-8", errors="replace")
            norm = buf.replace("\r", "\n")
            parts = norm.split("\n")
            if norm.endswith("\n"):
                ready, buf = parts[:-1], ""
            else:
                ready, buf = parts[:-1], parts[-1]
            for raw in ready:
                line = raw.strip()
                if line:
                    self._lines.put(line)
                    self._on_log("rx", line)

    def _drain_lines(self) -> None:
        while True:
            try:
                self._lines.get_nowait()
            except queue.Empty:
                return

    def write_bytes(self, payload: bytes, label: str) -> None:
        if not self.is_open() or self._serial is None:
            raise RuntimeError("串口未连接")
        total = 0
        try:
            with self._write_lock:
                for off in range(0, len(payload), SERIAL_WRITE_BURST_SIZE):
                    part = payload[off:off + SERIAL_WRITE_BURST_SIZE]
                    written = self._serial.write(part)
                    total += written
                    if written != len(part):
                        break
                    if off + len(part) < len(payload):
                        self._serial.flush()
                        time.sleep(SERIAL_WRITE_BURST_GAP_S)
                self._serial.flush()
        except Exception as exc:
            self.close()
            raise RuntimeError(f"串口写入失败，已断开，请检查端口占用或重新插拔后重连: {exc}") from exc
        if total != len(payload):
            raise RuntimeError(f"串口写入不完整: {total}/{len(payload)} bytes")

    def send_line(self, line: str) -> None:
        clean = line.strip()
        if not clean:
            raise RuntimeError("空命令")
        self.write_bytes((clean + "\n").encode("ascii"), clean)

    def wait_for(self, pattern: str, timeout: float, *,
                 regex: bool = False, collect_lines: bool = False,
                 ignore_unknown_command: bool = False) -> WaitResult:
        started = time.monotonic()
        deadline = started + timeout
        compiled = re.compile(pattern) if regex else None
        last_line = ""
        recent: list[str] = []
        while time.monotonic() < deadline:
            try:
                line = self._lines.get(timeout=0.05)
            except queue.Empty:
                continue
            last_line = line
            if collect_lines:
                recent.append(line)
                if len(recent) > 10:
                    recent.pop(0)
            matched = bool(compiled.search(line)) if compiled else pattern in line
            if matched:
                return WaitResult(line=line, elapsed_ms=int((time.monotonic() - started) * 1000))
            if ignore_unknown_command and line == "@ERR unknown_command":
                continue
            if line.startswith("@ERR") or line.startswith("@NACK"):
                raise RuntimeError(f"收到错误响应: {line}")
        elapsed = int((time.monotonic() - started) * 1000)
        tail = recent[-10:] if recent else [last_line or "无"]
        raise TimeoutError(f"等待 {pattern} 超时 {elapsed}ms，最近响应: {tail}")

    def transact_status(self, timeout: float) -> WaitResult:
        with self._command_lock:
            self._drain_lines()
            self.send_line("@STATUS")
            return self.wait_for("@STATUS", timeout, ignore_unknown_command=True)

    def send_control(self, command: str, expect: str, timeout: float) -> WaitResult:
        with self._command_lock:
            self._drain_lines()
            self.send_line(command)
            return self.wait_for(expect, timeout)

    def request_priority_control(
        self,
        command: str,
        expect: str,
        timeout: float,
        *,
        interrupt_upload: bool = True,
    ) -> bool:
        """Queue a control command to be sent between upload chunks."""
        control = PriorityControl(command.strip(), expect, timeout, interrupt_upload)
        if not control.command:
            raise RuntimeError("空控制命令")
        with self._priority_lock:
            if control.command == "@EXEC_STOP" and self._upload_paused:
                return False
            pending = self._priority_control
            if pending is None:
                self._priority_control = control
                return True
            if pending.command == control.command:
                return False
            if pending.command == "@ABORT":
                return False
            if control.command == "@ABORT":
                self._priority_control = control
                return True
            if TX_PRIORITY_CONTROL_OPPOSITES.get(control.command) == pending.command:
                self._priority_control = control
                return True
            if pending.command == "@FOCUS_OFF" and control.command == "@EXEC_STOP":
                self._priority_control = control
                return True
            if pending.command == "@EXEC_RESUME" and control.command == "@FOCUS_OFF":
                self._priority_control = control
                return True
            if pending.command == "@EXEC_STOP" and control.command == "@FOCUS_OFF":
                return False
            if pending.command == "@FOCUS_OFF" and control.command == "@EXEC_RESUME":
                return False
            return False

    def _pop_priority_control(self) -> Optional[PriorityControl]:
        with self._priority_lock:
            control = self._priority_control
            self._priority_control = None
            return control

    def _set_upload_paused(self, paused: bool) -> None:
        with self._priority_lock:
            self._upload_paused = paused

    def _is_upload_paused(self) -> bool:
        with self._priority_lock:
            return self._upload_paused

    def _dispatch_priority_control_locked(self) -> Optional[str]:
        control = self._pop_priority_control()
        if control is None:
            return None
        self._on_log("status", f"[CTRL_FAST] send {control.command}")
        code = TX_PRIORITY_CONTROL_CODES.get(control.command)
        if code is None:
            self.send_line(control.command)
        else:
            self.write_bytes(bytes((TX_UART_CONTROL_BYTE,)) + code, f"<control {control.command}>")
        result = self.wait_for(control.expect, control.timeout)
        self._on_log("status", f"[CTRL_FAST] ack {result.elapsed_ms}ms: {result.line}")
        if control.command == "@EXEC_STOP":
            self._set_upload_paused(True)
        elif control.command == "@EXEC_RESUME":
            self._set_upload_paused(False)
        elif control.command == "@ABORT":
            self._set_upload_paused(False)
        if control.interrupt_upload:
            raise UploadInterrupted(f"控制命令已发送，上传已停止: {control.command}")
        return control.command

    def _wait_while_upload_paused_locked(
        self,
        status_cb: Optional[Callable[[str, float], None]] = None,
        pct: float = -1,
    ) -> None:
        logged = False
        while self._is_upload_paused():
            if not logged:
                self._on_log("status", "[CTRL_FAST] 上传已暂停，等待恢复或取消")
                if status_cb:
                    status_cb("上传已暂停，等待恢复或取消", pct)
                logged = True
            self._dispatch_priority_control_locked()
            if self._is_upload_paused():
                time.sleep(0.05)
        if logged:
            self._on_log("status", "[CTRL_FAST] 上传已恢复")

    def _resync_tx_locked(self, timeout: float) -> WaitResult:
        """Abort any prior RX job and reset TX parsing in every UART mode.

        The caller must hold ``_command_lock``. ASCII CAN (0x18) is reserved as
        an out-of-band byte, so stale raw-data mode cannot consume it as G-code.
        TX only acknowledges after RX confirms PKT_JOB_ABORT.
        """
        self._drain_lines()
        self._on_log("status", "同步 TX/RX 任务状态")
        self.write_bytes(bytes((TX_UART_RESYNC_BYTE,)), "<resync 0x18>")
        result = self.wait_for("@OK resync rx=aborted", timeout, ignore_unknown_command=True)
        self._on_log("status", f"TX/RX 同步完成 {result.elapsed_ms}ms")
        return result

    def upload_job(self, job_id: int, gcode: bytes, timeout: float, *,
                   progress_cb: Optional[Callable[[str], None]] = None,
                   status_cb: Optional[Callable[[str, float], None]] = None,
                   preroll_bytes: int = 0,
                   start_on_preroll: bool = False,
                   enforce_job_size_limit: bool = True,
                   recover_tx: bool = True) -> None:
        if not gcode:
            raise RuntimeError("G-code 内容为空")
        if enforce_job_size_limit and len(gcode) > JOB_MAX_SIZE:
            raise RuntimeError(
                f"G-code 超过仅上传任务上限: {len(gcode)}/{JOB_MAX_SIZE} bytes"
            )
        if TX_UART_RESYNC_BYTE in gcode:
            raise RuntimeError("G-code 包含保留的 UART 同步控制字节 0x18")
        if TX_UART_CONTROL_BYTE in gcode:
            raise RuntimeError("G-code 包含保留的 UART 控制帧字节 0x19")
        if preroll_bytes > 0 and not start_on_preroll:
            raise RuntimeError("preroll 只能用于上传并执行，不能用于仅上传")

        crc = crc16_ccitt(gcode)
        total = len(gcode)
        t_begin = time.perf_counter()

        with self._command_lock:
            self._set_upload_paused(False)
            resync_timeout = max(timeout, TX_RESYNC_TIMEOUT_MIN_S)
            if recover_tx:
                self._resync_tx_locked(resync_timeout)
            else:
                self._drain_lines()

            transaction_started = False
            try:
                if status_cb:
                    status_cb("正在建立任务", 0)
                cmd = f"@BEGIN {job_id} {total} {crc:04x}"
                if start_on_preroll and preroll_bytes > 0:
                    cmd += f" preroll={preroll_bytes}"
                transaction_started = True
                self.send_line(cmd)
                self.wait_for(f"@DATA_READY job={job_id}", timeout)
                t_ready = time.perf_counter()
                self._on_log("status", f"DATA_READY {(t_ready-t_begin)*1000:.0f}ms")
                if status_cb:
                    status_cb("上传中 0/{} bytes".format(total), 0)

                offset = 0
                chunk_count = 0
                timing_write_ms_total = 0
                timing_ack_ms_total = 0
                timing_chunk_ms_total = 0
                t_data = time.perf_counter()
                preroll_done = False
                data_timeout = max(timeout, DATA_ACK_TIMEOUT_MIN_S)

                while offset < total:
                    self._dispatch_priority_control_locked()
                    self._wait_while_upload_paused_locked(
                        status_cb, offset * 100 // total if total else -1
                    )
                    end = min(offset + JOB_DATA_CHUNK_SIZE, total)
                    if (
                        start_on_preroll and preroll_bytes > 0 and not preroll_done
                        and offset < preroll_bytes < end
                    ):
                        end = preroll_bytes
                    chunk = gcode[offset:end]
                    chunk_index = chunk_count + 1
                    chunk_t0 = time.perf_counter()
                    write_t0 = chunk_t0
                    self.write_bytes(chunk, f"<chunk> off={offset} len={len(chunk)}")
                    write_ms = int((time.perf_counter() - write_t0) * 1000)
                    try:
                        ack = self.wait_for(
                            rf"@ACK type=2 .*status=0 .*offset={end}\b",
                            data_timeout, regex=True,
                        )
                    except RuntimeError as exc:
                        if "@ERR unknown_command" in str(exc):
                            raise RuntimeError(
                                "TX 在上传数据阶段返回 unknown_command。"
                                "如果这是暂停执行后出现的，说明当前 TX 固件可能仍是旧版："
                                "旧版 EXEC_STOP 会退出 data_mode，后续 G-code 被当作命令解析。"
                                "请烧录最新 ws63-liteos-app_tx_all.fwpkg 后再测试暂停/恢复。"
                            ) from exc
                        raise
                    except TimeoutError:
                        self._dispatch_priority_control_locked()
                        raise
                    chunk_ms = int((time.perf_counter() - chunk_t0) * 1000)
                    ack_wait_ms = ack.elapsed_ms
                    timing_write_ms_total += write_ms
                    timing_ack_ms_total += ack_wait_ms
                    timing_chunk_ms_total += chunk_ms
                    offset = end
                    chunk_count += 1
                    has_preroll = "preroll=1" in ack.line
                    is_last = offset >= total
                    if chunk_count <= 3 or has_preroll or is_last:
                        self._on_log("status", f"ACK off={offset} preroll={1 if has_preroll else 0}")
                    if (
                        chunk_index <= HOST_TIMING_FIRST_CHUNKS
                        or chunk_index % HOST_TIMING_EVERY_CHUNKS == 0
                        or chunk_ms >= HOST_TIMING_SLOW_CHUNK_MS
                        or has_preroll
                        or is_last
                    ):
                        self._on_log(
                            "status",
                            f"[HOST_TIMING] chunk={chunk_index} off={offset} len={len(chunk)} "
                            f"write_ms={write_ms} ack_wait_ms={ack_wait_ms} "
                            f"total_ms={chunk_ms}",
                        )
                    pct = offset * 100 // total
                    if status_cb:
                        status_cb(f"上传中 {offset}/{total} bytes", pct)
                    if progress_cb and (chunk_count % 100 == 0 or is_last):
                        progress_cb(f"上传中 {offset}/{total} ({pct}%)")
                    self._dispatch_priority_control_locked()
                    self._wait_while_upload_paused_locked(status_cb, pct)
                    if start_on_preroll and preroll_bytes > 0 and not preroll_done and has_preroll:
                        preroll_done = True
                        self._on_log("status", f"PREROLL offset={offset} elapsed={int((time.perf_counter()-t_begin)*1000)}ms")
                        if status_cb:
                            status_cb("预缓冲完成，启动执行", pct)
                        self.send_line(f"@EXEC_START {job_id}")
                        self.wait_for("@ACK type=16", timeout)
                        self.send_line("@DATA_RESUME")
                        self.wait_for("@OK data_resume", timeout)
                        self._on_log("status", "DATA_RESUME OK")
                        if status_cb:
                            status_cb(f"继续上传 {offset}/{total} bytes", pct)

                data_elapsed = time.perf_counter() - t_data
                rate = total / data_elapsed if data_elapsed > 0 else 0
                self._on_log("status", f"DATA_DONE {data_elapsed*1000:.0f}ms rate={rate:.0f}B/s chunks={chunk_count}")
                if chunk_count > 0:
                    self._on_log(
                        "status",
                        f"[HOST_TIMING_SUM] chunks={chunk_count} "
                        f"avg_write_ms={timing_write_ms_total / chunk_count:.1f} "
                        f"avg_ack_wait_ms={timing_ack_ms_total / chunk_count:.1f} "
                        f"avg_chunk_ms={timing_chunk_ms_total / chunk_count:.1f}",
                    )

                # The final DATA ACK only confirms that TX accepted the UART
                # bytes. JOB_READY is the transaction commit barrier: TX has
                # sent JOB_END and RX has acknowledged the complete job.
                commit_timeout = max(timeout, JOB_COMMIT_TIMEOUT_MIN_S)
                self._on_log(
                    "status",
                    f"JOB_COMMIT_WAIT job={job_id} timeout={commit_timeout:.1f}s",
                )
                self.wait_for(f"@JOB_READY job={job_id}", commit_timeout)
                total_elapsed = time.perf_counter() - t_begin
                total_rate = total / total_elapsed if total_elapsed > 0 else 0
                self._on_log(
                    "status",
                    f"JOB_READY commit=confirmed total={total_elapsed*1000:.0f}ms "
                    f"rate={total_rate:.0f}B/s",
                )
            except UploadInterrupted:
                raise
            except Exception:
                if transaction_started:
                    try:
                        self._resync_tx_locked(resync_timeout)
                    except Exception as resync_exc:
                        self._on_log("error", f"上传失败后的安全同步也失败: {resync_exc}")
                raise


class SerialLogMonitor:
    """Read-only serial port monitor for TX/RX debug logs."""

    def __init__(self, name: str, on_log: Callable[[str, str], None]) -> None:
        self._name = name
        self._on_log = on_log
        self._serial: Optional[serial.Serial] = None
        self._reader: Optional[threading.Thread] = None
        self._running = threading.Event()
        self.port: Optional[str] = None

    def is_open(self) -> bool:
        return self._serial is not None and self._serial.is_open

    def open(self, port: str, baud: int) -> None:
        self.close()
        self.port = port
        self._serial = serial.Serial(port, baudrate=baud, timeout=0.05, write_timeout=0.2)
        self._serial.reset_input_buffer()
        self._running.set()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self._on_log("status", f"{self._name} 已监听 {port} @ {baud}")

    def close(self) -> None:
        self._running.clear()
        if self._reader and self._reader.is_alive():
            self._reader.join(timeout=0.5)
        self._reader = None
        if self._serial:
            try:
                self._serial.close()
            except Exception:
                pass
        self._serial = None
        self.port = None

    def _read_loop(self) -> None:
        assert self._serial is not None
        buf = ""
        while self._running.is_set() and self._serial and self._serial.is_open:
            try:
                chunk = self._serial.read(self._serial.in_waiting or 1)
            except Exception as exc:
                self._on_log("error", f"{self._name} 读取失败: {exc}")
                break
            if not chunk:
                continue
            buf += chunk.decode("utf-8", errors="replace")
            norm = buf.replace("\r", "\n")
            parts = norm.split("\n")
            if norm.endswith("\n"):
                ready, buf = parts[:-1], ""
            else:
                ready, buf = parts[:-1], parts[-1]
            for raw in ready:
                line = raw.strip()
                if line:
                    self._on_log(self._name.lower(), line)
