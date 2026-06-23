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
JOB_DATA_CHUNK_SIZE = 214
JOB_MAX_SIZE = 131072
GCODE_LINE_MAX_BYTES = 120  # RX SLE_JOB_GCODE_LINE_MAX=128, leave 8-byte safety margin
DATA_ACK_TIMEOUT_MIN_S = 10.0
JOB_COMMIT_TIMEOUT_MIN_S = 12.0
TX_RESYNC_TIMEOUT_MIN_S = 12.0
TX_UART_RESYNC_BYTE = 0x18
SERIAL_WRITE_BURST_SIZE = 128
SERIAL_WRITE_BURST_GAP_S = 0.001
JOB_PREROLL_BYTES = 4096

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
class RxStatus:
    state: int
    error: int
    job_id: int
    received_size: int
    job_total: int
    cache_free: int


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
        if total != len(payload):
            raise RuntimeError(f"串口写入不完整: {total}/{len(payload)} bytes")

    def send_line(self, line: str) -> None:
        clean = line.strip()
        if not clean:
            raise RuntimeError("空命令")
        self.write_bytes((clean + "\n").encode("ascii"), clean)

    def wait_for(self, pattern: str, timeout: float, *,
                 regex: bool = False, collect_lines: bool = False) -> WaitResult:
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
            if line.startswith("@ERR") or line.startswith("@NACK"):
                raise RuntimeError(f"收到错误响应: {line}")
        elapsed = int((time.monotonic() - started) * 1000)
        tail = recent[-10:] if recent else [last_line or "无"]
        raise TimeoutError(f"等待 {pattern} 超时 {elapsed}ms，最近响应: {tail}")

    def transact_status(self, timeout: float) -> WaitResult:
        with self._command_lock:
            self._drain_lines()
            self.send_line("@STATUS")
            return self.wait_for("@STATUS", timeout)

    def send_control(self, command: str, expect: str, timeout: float) -> WaitResult:
        with self._command_lock:
            self._drain_lines()
            self.send_line(command)
            return self.wait_for(expect, timeout)

    def _resync_tx_locked(self, timeout: float) -> WaitResult:
        """Abort any prior RX job and reset TX parsing in every UART mode.

        The caller must hold ``_command_lock``. ASCII CAN (0x18) is reserved as
        an out-of-band byte, so stale raw-data mode cannot consume it as G-code.
        TX only acknowledges after RX confirms PKT_JOB_ABORT.
        """
        self._drain_lines()
        self._on_log("status", "同步 TX/RX 任务状态")
        self.write_bytes(bytes((TX_UART_RESYNC_BYTE,)), "<resync 0x18>")
        result = self.wait_for("@OK resync rx=aborted", timeout)
        self._on_log("status", f"TX/RX 同步完成 {result.elapsed_ms}ms")
        return result

    def upload_job(self, job_id: int, gcode: bytes, timeout: float, *,
                   progress_cb: Optional[Callable[[str], None]] = None,
                   status_cb: Optional[Callable[[str, float], None]] = None,
                   preroll_bytes: int = 0,
                   start_on_preroll: bool = False,
                   recover_tx: bool = True) -> None:
        if not gcode:
            raise RuntimeError("G-code 内容为空")
        if len(gcode) > JOB_MAX_SIZE:
            raise RuntimeError(
                f"G-code 超过 TX/RX 任务上限: {len(gcode)}/{JOB_MAX_SIZE} bytes"
            )
        if TX_UART_RESYNC_BYTE in gcode:
            raise RuntimeError("G-code 包含保留的 UART 同步控制字节 0x18")
        if preroll_bytes > 0 and not start_on_preroll:
            raise RuntimeError("preroll 只能用于上传并执行，不能用于仅上传")

        crc = crc16_ccitt(gcode)
        total = len(gcode)
        t_begin = time.perf_counter()

        with self._command_lock:
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
                t_data = time.perf_counter()
                preroll_done = False
                data_timeout = max(timeout, DATA_ACK_TIMEOUT_MIN_S)

                while offset < total:
                    end = min(offset + JOB_DATA_CHUNK_SIZE, total)
                    chunk = gcode[offset:end]
                    self.write_bytes(chunk, f"<chunk> off={offset} len={len(chunk)}")
                    ack = self.wait_for(
                        rf"@ACK type=2 .*status=0 .*offset={end}\b",
                        data_timeout, regex=True,
                    )
                    offset = end
                    chunk_count += 1
                    has_preroll = "preroll=1" in ack.line
                    is_last = offset >= total
                    if chunk_count <= 12 or has_preroll or is_last:
                        self._on_log("status", f"ACK off={offset} preroll={1 if has_preroll else 0}")
                    pct = offset * 100 // total
                    if status_cb:
                        status_cb(f"上传中 {offset}/{total} bytes", pct)
                    if progress_cb and (chunk_count % 100 == 0 or is_last):
                        progress_cb(f"上传中 {offset}/{total} ({pct}%)")
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
