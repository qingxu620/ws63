from __future__ import annotations

import queue
import re
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Optional, TextIO

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - runtime dependency
    raise SystemExit("缺少 pyserial，请先运行: pip install pyserial") from exc

import tkinter as tk
from tkinter import filedialog, messagebox, ttk


BAUD_DEFAULT = 115200
CMD_TIMEOUT_DEFAULT = 8.0
UPLOAD_TIMEOUT_DEFAULT = 20.0
DEFAULT_JOB_ID = 1
EXEC_START_DELAY_AFTER_UPLOAD_S = 0.2
JOB_DATA_CHUNK_SIZE = 214
SERIAL_WRITE_BURST_SIZE = 128
SERIAL_WRITE_BURST_GAP_S = 0.001
LOG_PUMP_MAX_LINES = 120
PROGRESS_PUMP_MAX_ITEMS = 20
LOG_TEXT_MAX_LINES = 4000
LOG_TEXT_TRIM_LINES = 500
LOG_FILE_FLUSH_LINES = 40
JOB_PREROLL_BYTES = 4096
RX_EXEC_DONE_RE = re.compile(
    r"\[JOB_EXEC\]\s+done\s+job=(\d+)\s+lines=(\d+)\s+x_um=(-?\d+)\s+y_um=(-?\d+)"
)
SHOW_ALWAYS_TOKENS = (
    "ERROR",
    "NACK",
    "@NACK",
    "@ERR",
    "TIMEOUT",
    "ABORT",
    "REJECT",
    "SAFE_STOP",
    "EXEC_DONE",
    "[JOB_EXEC] start",
    "[JOB_EXEC] done",
    "JOB_READY",
    "DATA_DONE",
    "EXEC_START ACK",
    "上传完成",
    "开始上传",
    "开始执行",
    "执行完成",
    "JOB_SLE_WRITE_CFM_TIMEOUT",
    "JOB_EXEC_WAIT_TIMEOUT",
    "JOB_RX_NACK_SEND",
    "JOB_DATA_REJECT",
    "JOB_CACHE_WRITE_REJECT",
)
HIDE_UI_TOKENS = (
    "ACK_PARSE",
    "[JOB_DATA_IN]",
    "[JOB_DATA_HDR]",
    "[JOB_DATA_RESULT]",
    "[JOB_TX_DATA_ACK_PRE]",
    "[JOB_TX_HOST_ACK]",
    "[TX_ACK]",
    "[RX_ACK]",
    "[JOB_EXEC_STREAM_THROTTLE]",
    "[JOB_EXEC] line=",
    "@ACK type=2",
    "update ssap send report handle",
)


def now_text() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def compact_text(text: str, limit: int = 220) -> str:
    clean = text.replace("\r", "\\r").replace("\n", "\\n")
    if len(clean) <= limit:
        return clean
    return clean[: limit - 3] + "..."


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
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return text


@dataclass
class WaitResult:
    line: str
    elapsed_ms: int


class SleJobSerialClient:
    def __init__(self, on_log: Callable[[str, str], None]) -> None:
        self._on_log = on_log
        self._serial: Optional[serial.Serial] = None
        self._reader: Optional[threading.Thread] = None
        self._running = threading.Event()
        self._lines: "queue.Queue[str]" = queue.Queue()
        self._write_lock = threading.Lock()
        self._command_lock = threading.Lock()

    @staticmethod
    def available_ports() -> list[str]:
        ports = []
        for port in list_ports.comports():
            desc = getattr(port, "description", "") or "Serial"
            ports.append(f"{port.device} - {desc}")
        return ports

    @staticmethod
    def port_device(port_display: str) -> str:
        return port_display.split(" - ", 1)[0].strip()

    def is_open(self) -> bool:
        return self._serial is not None and self._serial.is_open

    def open(self, port: str, baud: int) -> None:
        self.close()
        self._serial = serial.Serial(port, baudrate=baud, timeout=0.05, write_timeout=2.0)
        self._serial.reset_input_buffer()
        self._serial.reset_output_buffer()
        self._running.set()
        self._reader = threading.Thread(target=self._read_loop, name="sle-job-uart-reader", daemon=True)
        self._reader.start()
        self._on_log("status", f"已连接 {port} @ {baud}")

    def close(self) -> None:
        self._running.clear()
        if self._reader is not None and self._reader.is_alive():
            self._reader.join(timeout=0.5)
        self._reader = None
        if self._serial is not None:
            try:
                self._serial.close()
            except serial.SerialException:
                pass
        self._serial = None

    @property
    def port(self) -> Optional[str]:
        if self._serial is None:
            return None
        return self._serial.port

    def _read_loop(self) -> None:
        assert self._serial is not None
        buffer = ""
        while self._running.is_set() and self._serial is not None and self._serial.is_open:
            try:
                chunk = self._serial.read(self._serial.in_waiting or 1)
            except serial.SerialException as exc:
                self._on_log("error", f"串口读取失败: {exc}")
                break
            if not chunk:
                continue
            text = chunk.decode("utf-8", errors="replace")
            buffer += text
            normalized = buffer.replace("\r", "\n")
            parts = normalized.split("\n")
            if normalized.endswith("\n"):
                ready, buffer = parts[:-1], ""
            else:
                ready, buffer = parts[:-1], parts[-1]
            for raw in ready:
                line = raw.strip()
                if not line:
                    continue
                self._lines.put(line)
                self._on_log("rx", line)

    def _drain_lines(self) -> None:
        while True:
            try:
                self._lines.get_nowait()
            except queue.Empty:
                return

    def write_bytes(self, payload: bytes, label: str, verbose: bool = False) -> None:
        if not self.is_open() or self._serial is None:
            raise RuntimeError("串口未连接")
        written_total = 0
        with self._write_lock:
            for offset in range(0, len(payload), SERIAL_WRITE_BURST_SIZE):
                part = payload[offset : offset + SERIAL_WRITE_BURST_SIZE]
                written = self._serial.write(part)
                written_total += written
                if written != len(part):
                    break
                if offset + len(part) < len(payload):
                    self._serial.flush()
                    time.sleep(SERIAL_WRITE_BURST_GAP_S)
            self._serial.flush()
        if written_total != len(payload):
            raise RuntimeError(f"串口写入不完整: {written_total}/{len(payload)} bytes")
        if verbose:
            self._on_log("tx", f"{label} uart={written_total}/{len(payload)}")

    def send_line(self, line: str) -> None:
        clean = line.strip()
        if not clean:
            raise RuntimeError("空命令")
        self.write_bytes((clean + "\n").encode("ascii"), clean)

    def wait_for(self, pattern: str, timeout: float, *, regex: bool = False,
                 collect_lines: bool = False) -> WaitResult:
        started = time.monotonic()
        deadline = started + timeout
        compiled = re.compile(pattern) if regex else None
        last_line = ""
        recent_lines: list[str] = []
        while time.monotonic() < deadline:
            try:
                line = self._lines.get(timeout=0.05)
            except queue.Empty:
                continue
            last_line = line
            if collect_lines:
                recent_lines.append(line)
                if len(recent_lines) > 10:
                    recent_lines.pop(0)
            matched = bool(compiled.search(line)) if compiled is not None else pattern in line
            if matched:
                return WaitResult(line=line, elapsed_ms=int((time.monotonic() - started) * 1000))
            if line.startswith("@ERR") or line.startswith("@NACK"):
                raise RuntimeError(f"收到错误响应: {line}")
        elapsed_ms = int((time.monotonic() - started) * 1000)
        tail = recent_lines[-10:] if recent_lines else [last_line or "无"]
        raise TimeoutError(f"等待 {pattern} 超时，耗时 {elapsed_ms} ms，最近响应: {tail}")

    def transact_status(self, timeout: float) -> WaitResult:
        with self._command_lock:
            self._drain_lines()
            self.send_line("@STATUS")
            return self.wait_for("@STATUS", timeout)

    def send_control(self, command: str, expect_pattern: str, timeout: float) -> WaitResult:
        with self._command_lock:
            self._drain_lines()
            self.send_line(command)
            return self.wait_for(expect_pattern, timeout)

    def upload_job(self, job_id: int, gcode: bytes, timeout: float, wait_ready: bool = True,
                   progress_cb: Optional[Callable[[str], None]] = None,
                   status_cb: Optional[Callable[[str, float], None]] = None,
                   total: int = 0, preroll_bytes: int = 0) -> None:
        if not gcode:
            raise RuntimeError("G-code 内容为空")

        crc = crc16_ccitt(gcode)
        total_size = len(gcode)
        t_begin = time.perf_counter()
        with self._command_lock:
            self._drain_lines()
            if status_cb:
                status_cb("正在建立任务", 0)
            if preroll_bytes > 0:
                self.send_line(f"@BEGIN {job_id} {total_size} {crc:04x} preroll={preroll_bytes}")
            else:
                self.send_line(f"@BEGIN {job_id} {total_size} {crc:04x}")
            ready = self.wait_for(f"@DATA_READY job={job_id}", timeout)
            t_ready = time.perf_counter()
            self._on_log("status", f"DATA_READY total={(t_ready - t_begin) * 1000:.0f} ms")
            if status_cb:
                status_cb(f"上传中 0/{total_size} bytes", 0)
            offset = 0
            chunk_count = 0
            t_data_start = time.perf_counter()
            preroll_done = False
            try:
                while offset < total_size:
                    end = min(offset + JOB_DATA_CHUNK_SIZE, total_size)
                    chunk = gcode[offset:end]
                    self.write_bytes(
                        chunk,
                        f"<raw gcode chunk> off={offset} len={len(chunk)} next={end}/{total_size} crc=0x{crc:04x}",
                    )
                    ack = self.wait_for(
                        rf"@ACK type=2 .*status=0 .*offset={end}\b",
                        timeout,
                        regex=True,
                    )
                    offset = end
                    chunk_count += 1
                    has_preroll = "preroll=1" in ack.line
                    is_last = (offset >= total_size)
                    should_log = (chunk_count <= 12) or has_preroll or is_last
                    if should_log:
                        self._on_log("status", f"ACK_PARSE raw=\"{ack.line}\" offset={offset} preroll={1 if has_preroll else 0}")
                    pct = offset * 100 // total_size
                    if status_cb:
                        status_cb(f"上传中 {offset}/{total_size} bytes", pct)
                    if progress_cb and (chunk_count % 100 == 0 or offset >= total_size):
                        progress_cb(f"上传中 {offset}/{total_size} ({pct}%)")
                    if preroll_bytes > 0 and not preroll_done and has_preroll:
                        preroll_done = True
                        t_preroll = time.perf_counter()
                        self._on_log("status", f"PREROLL_TRIGGER source=ack_preroll offset={offset} elapsed={(t_preroll - t_begin) * 1000:.0f} ms")
                        if status_cb:
                            status_cb("预缓冲完成，启动执行", offset * 100 // total_size)
                        self._on_log("status", f"SEND_EXEC_START reason=preroll_ack job={job_id} offset={offset}")
                        self.send_line(f"@EXEC_START {job_id}")
                        self._on_log("status", f"WAIT_EXEC_ACK begin timeout={timeout} ms")
                        exec_ack = self.wait_for("@ACK type=16", timeout, collect_lines=True)
                        t_exec_ack = time.perf_counter()
                        self._on_log("status", f"STREAM_EXEC_START ACK {(t_exec_ack - t_preroll) * 1000:.0f} ms")
                        self.send_line("@DATA_RESUME")
                        resume_ok = self.wait_for("@OK data_resume", timeout)
                        self._on_log("status", "DATA_RESUME OK")
                        if status_cb:
                            status_cb(f"继续上传 {offset}/{total_size} bytes", pct)
            except Exception as exc:
                if preroll_done:
                    self._on_log("error", f"预缓冲上传失败，发送 ABORT: {exc}")
                    try:
                        self.send_line("@ABORT")
                    except Exception:
                        pass
                    if status_cb:
                        status_cb("上传失败，已中止 RX 执行", 0)
                raise
            t_data_done = time.perf_counter()
            data_elapsed = t_data_done - t_data_start
            data_rate = total_size / data_elapsed if data_elapsed > 0 else 0
            self._on_log("status", f"DATA_DONE data={data_elapsed * 1000:.0f} ms "
                          f"rate={data_rate:.0f} B/s chunks={chunk_count} size={total_size}")
            if status_cb:
                status_cb("数据已发送，等待 RX 存储确认", 99)
            if wait_ready:
                done = self.wait_for(f"@JOB_READY job={job_id}", timeout)
                t_end = time.perf_counter()
                total_elapsed = t_end - t_begin
                total_rate = total_size / total_elapsed if total_elapsed > 0 else 0
                self._on_log("status", f"JOB_READY total={total_elapsed * 1000:.0f} ms "
                              f"rate={total_rate:.0f} B/s size={total_size} chunks={chunk_count}")
            else:
                self._on_log("status", f"上传完成，跳过 JOB_READY 等待")


class SerialLogMonitor:
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
        self._reader = threading.Thread(target=self._read_loop, name=f"{self._name}-reader", daemon=True)
        self._reader.start()
        self._on_log("status", f"{self._name} 已监听 {port} @ {baud}")

    def close(self) -> None:
        self._running.clear()
        if self._reader is not None and self._reader.is_alive():
            self._reader.join(timeout=0.5)
        self._reader = None
        if self._serial is not None:
            try:
                self._serial.close()
            except serial.SerialException:
                pass
        self._serial = None
        self.port = None

    def _read_loop(self) -> None:
        assert self._serial is not None
        buffer = ""
        while self._running.is_set() and self._serial is not None and self._serial.is_open:
            try:
                chunk = self._serial.read(self._serial.in_waiting or 1)
            except serial.SerialException as exc:
                self._on_log("error", f"{self._name} 读取失败: {exc}")
                break
            if not chunk:
                continue
            text = chunk.decode("utf-8", errors="replace")
            buffer += text
            normalized = buffer.replace("\r", "\n")
            parts = normalized.split("\n")
            if normalized.endswith("\n"):
                ready, buffer = parts[:-1], ""
            else:
                ready, buffer = parts[:-1], parts[-1]
            for raw in ready:
                line = raw.strip()
                if line:
                    self._on_log(self._name.lower(), line)


class SleJobHostApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("WS63 SLE Job G-code Uploader")
        self.geometry("1220x780")
        self.minsize(1000, 640)

        self.log_queue: "queue.Queue[tuple[str, str]]" = queue.Queue()
        self.progress_queue: "queue.Queue[str]" = queue.Queue()
        self.task_status_queue: "queue.Queue[tuple[str, int]]" = queue.Queue()  # (status_text, progress_value)
        self.client = SleJobSerialClient(self.enqueue_log)
        self.tx_log_monitor = SerialLogMonitor("tx_log", self.enqueue_log)
        self.rx_log_monitor = SerialLogMonitor("rx_log", self.enqueue_log)
        self.worker_thread: Optional[threading.Thread] = None
        self.log_path: Optional[Path] = None
        self.log_file: Optional[TextIO] = None
        self.log_file_pending = 0
        self.log_line_count = 0
        self.loaded_file: Optional[Path] = None

        self._build_ui()
        self.refresh_ports()
        self.after(50, self._pump_logs)
        self.enqueue_log(
            "status",
            f"上位机配置: job_data_chunk={JOB_DATA_CHUNK_SIZE}B "
            f"serial_burst={SERIAL_WRITE_BURST_SIZE}B gap={int(SERIAL_WRITE_BURST_GAP_S * 1000)}ms",
        )
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    def _build_ui(self) -> None:
        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)

        top = ttk.Frame(self, padding=8)
        top.grid(row=0, column=0, sticky="ew")
        top.columnconfigure(1, weight=1)
        top.columnconfigure(8, weight=1)

        ttk.Label(top, text="TX 串口").grid(row=0, column=0, padx=(0, 6))
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=34, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky="ew", padx=(0, 6))
        ttk.Button(top, text="刷新", command=self.refresh_ports).grid(row=0, column=2, padx=(0, 12))

        ttk.Label(top, text="波特率").grid(row=0, column=3, padx=(0, 6))
        self.baud_var = tk.StringVar(value=str(BAUD_DEFAULT))
        ttk.Entry(top, textvariable=self.baud_var, width=10).grid(row=0, column=4, padx=(0, 12))

        self.connect_button = ttk.Button(top, text="连接", command=self.toggle_connection)
        self.connect_button.grid(row=0, column=5, padx=(0, 6))
        ttk.Button(top, text="保存日志", command=self.save_log_as).grid(row=0, column=6)

        ttk.Label(top, text="TX日志").grid(row=1, column=0, padx=(0, 6), pady=(6, 0))
        self.tx_log_port_var = tk.StringVar()
        self.tx_log_combo = ttk.Combobox(top, textvariable=self.tx_log_port_var, width=34, state="readonly")
        self.tx_log_combo.grid(row=1, column=1, sticky="ew", padx=(0, 6), pady=(6, 0))
        self.tx_log_button = ttk.Button(top, text="监听TX日志", command=self.toggle_tx_log_monitor)
        self.tx_log_button.grid(row=1, column=2, padx=(0, 12), pady=(6, 0))

        ttk.Label(top, text="RX日志").grid(row=1, column=3, padx=(0, 6), pady=(6, 0))
        self.rx_log_port_var = tk.StringVar()
        self.rx_log_combo = ttk.Combobox(top, textvariable=self.rx_log_port_var, width=34, state="readonly")
        self.rx_log_combo.grid(row=1, column=4, sticky="ew", padx=(0, 6), pady=(6, 0))
        self.rx_log_button = ttk.Button(top, text="监听RX日志", command=self.toggle_rx_log_monitor)
        self.rx_log_button.grid(row=1, column=5, padx=(0, 6), pady=(6, 0))

        main = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        main.grid(row=1, column=0, sticky="nsew", padx=8, pady=(0, 8))

        left = ttk.Frame(main, padding=8)
        right = ttk.Frame(main, padding=8)
        main.add(left, weight=3)
        main.add(right, weight=2)

        left.rowconfigure(1, weight=1)
        left.columnconfigure(0, weight=1)

        header = ttk.Frame(left)
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(1, weight=1)
        ttk.Label(header, text="G-code 任务内容").grid(row=0, column=0, sticky="w")
        self.file_var = tk.StringVar(value="未加载文件")
        ttk.Label(header, textvariable=self.file_var).grid(row=0, column=1, sticky="e")

        self.gcode_text = tk.Text(left, height=18, wrap="none", undo=True)
        self.gcode_text.grid(row=1, column=0, sticky="nsew", pady=(4, 8))
        self.gcode_text.insert("1.0", "G90\nM3 S50\nG1 X10 Y10 F1000\nM5\n")

        controls = ttk.Frame(left)
        controls.grid(row=2, column=0, sticky="ew")
        controls.columnconfigure(10, weight=1)
        ttk.Button(controls, text="打开 G-code", command=self.load_file).grid(row=0, column=0, padx=(0, 6))
        ttk.Button(controls, text="上传任务", command=self.upload_job).grid(row=0, column=1, padx=(0, 6))
        ttk.Button(controls, text="上传并执行", command=self.upload_and_start).grid(row=0, column=2, padx=(0, 6))
        ttk.Button(controls, text="执行", command=self.exec_start).grid(row=0, column=3, padx=(0, 6))
        ttk.Button(controls, text="停止", command=self.exec_stop).grid(row=0, column=4, padx=(0, 6))
        ttk.Button(controls, text="放弃任务", command=self.abort_job).grid(row=0, column=5, padx=(0, 12))

        ttk.Label(controls, text="Job ID").grid(row=0, column=6, padx=(0, 4))
        self.job_id_var = tk.StringVar(value=str(DEFAULT_JOB_ID))
        ttk.Entry(controls, textvariable=self.job_id_var, width=6).grid(row=0, column=7, padx=(0, 8))
        ttk.Label(controls, text="超时(s)").grid(row=0, column=8, padx=(0, 4))
        self.timeout_var = tk.StringVar(value=str(UPLOAD_TIMEOUT_DEFAULT))
        ttk.Entry(controls, textvariable=self.timeout_var, width=6).grid(row=0, column=9, padx=(0, 8))
        self.progress_var = tk.StringVar(value="待上传")
        ttk.Label(controls, textvariable=self.progress_var).grid(row=0, column=10, sticky="e")

        mode_frame = ttk.LabelFrame(left, text="执行模式", padding=6)
        mode_frame.grid(row=3, column=0, sticky="ew", pady=(4, 4))
        self.exec_mode_var = tk.StringVar(value="normal")
        ttk.Radiobutton(mode_frame, text="普通模式：完整下发后执行",
                        variable=self.exec_mode_var, value="normal",
                        command=self._on_mode_change).grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Radiobutton(mode_frame, text="预缓冲模式：边收边执行",
                        variable=self.exec_mode_var, value="preroll",
                        command=self._on_mode_change).grid(row=1, column=0, sticky="w")
        ttk.Label(mode_frame, text="预缓冲大小(bytes):").grid(row=1, column=1, padx=(12, 4), sticky="w")
        self.preroll_bytes_var = tk.StringVar(value=str(JOB_PREROLL_BYTES))
        self.preroll_entry = ttk.Entry(mode_frame, textvariable=self.preroll_bytes_var, width=8, state="disabled")
        self.preroll_entry.grid(row=1, column=2, padx=(0, 6), sticky="w")

        status_frame = ttk.Frame(left)
        status_frame.grid(row=4, column=0, sticky="ew", pady=(4, 8))
        status_frame.columnconfigure(1, weight=1)

        ttk.Label(status_frame, text="任务状态:").grid(row=0, column=0, padx=(0, 6))
        self.task_status_var = tk.StringVar(value="空闲")
        self.task_status_label = ttk.Label(status_frame, textvariable=self.task_status_var,
                                           font=("", 10, "bold"))
        self.task_status_label.grid(row=0, column=1, sticky="w")

        self.task_progress = ttk.Progressbar(status_frame, mode="determinate", length=200)
        self.task_progress.grid(row=0, column=2, padx=(12, 0), sticky="e")
        self.task_progress["value"] = 0

        quick = ttk.Frame(left)
        quick.grid(row=5, column=0, sticky="ew", pady=(8, 0))
        ttk.Button(quick, text="@STATUS", command=self.query_status).grid(row=0, column=0, padx=(0, 6))
        ttk.Button(quick, text="@EXEC_START", command=self.exec_start).grid(row=0, column=1, padx=(0, 6))
        ttk.Button(quick, text="@EXEC_STOP", command=self.exec_stop).grid(row=0, column=2, padx=(0, 6))
        ttk.Button(quick, text="@ABORT", command=self.abort_job).grid(row=0, column=3, padx=(0, 6))

        right.rowconfigure(1, weight=1)
        right.columnconfigure(0, weight=1)
        log_header = ttk.Frame(right)
        log_header.grid(row=0, column=0, sticky="ew")
        log_header.columnconfigure(0, weight=1)
        ttk.Label(log_header, text="通信日志").grid(row=0, column=0, sticky="w")
        self.show_diag_logs_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(log_header, text="诊断日志",
                        variable=self.show_diag_logs_var).grid(row=0, column=1, padx=(0, 8), sticky="e")
        ttk.Button(log_header, text="清空日志", command=self.clear_log).grid(row=0, column=2, sticky="e")
        self.log_text = tk.Text(right, height=20, wrap="word", state="disabled")
        self.log_text.grid(row=1, column=0, sticky="nsew", pady=(4, 0))
        self.log_text.tag_configure("tx", foreground="#0B6E4F")
        self.log_text.tag_configure("rx", foreground="#1B4D89")
        self.log_text.tag_configure("error", foreground="#B00020")
        self.log_text.tag_configure("status", foreground="#7A4E00")
        self.log_text.tag_configure("tx_log", foreground="#6A1B9A")
        self.log_text.tag_configure("rx_log", foreground="#00695C")

    def enqueue_log(self, kind: str, message: str) -> None:
        self.log_queue.put((kind, message))

    def enqueue_progress(self, text: str) -> None:
        self.progress_queue.put(text)

    def enqueue_task_status(self, status: str, progress: float = -1) -> None:
        self.task_status_queue.put((status, int(progress)))

    def _pump_logs(self) -> None:
        try:
            processed = 0
            while processed < LOG_PUMP_MAX_LINES:
                try:
                    kind, message = self.log_queue.get_nowait()
                except queue.Empty:
                    break
                self.append_log(kind, message)
                processed += 1

            progress_processed = 0
            while progress_processed < PROGRESS_PUMP_MAX_ITEMS:
                try:
                    text = self.progress_queue.get_nowait()
                except queue.Empty:
                    break
                self.progress_var.set(text)
                progress_processed += 1

            task_status_processed = 0
            while task_status_processed < PROGRESS_PUMP_MAX_ITEMS:
                try:
                    status_text, progress_val = self.task_status_queue.get_nowait()
                except queue.Empty:
                    break
                self.task_status_var.set(status_text)
                if progress_val >= 0:
                    self.task_progress["value"] = progress_val
                task_status_processed += 1
        except Exception:
            pass
        try:
            delay_ms = 10 if (not self.log_queue.empty() or not self.progress_queue.empty() or
                              not self.task_status_queue.empty()) else 100
            self.after(delay_ms, self._pump_logs)
        except Exception:
            pass

    def _log_kind_display(self, kind: str) -> str:
        mapping = {
            "tx_log": "TX日志",
            "rx_log": "RX日志",
            "tx": "TX",
            "rx": "RX",
            "status": "STATUS",
            "error": "ERROR",
        }
        return mapping.get(kind, kind.upper())

    def _should_show_log(self, kind: str, message: str) -> bool:
        if getattr(self, "show_diag_logs_var", None) is not None and self.show_diag_logs_var.get():
            return True
        if kind == "error":
            return True
        text = str(message)
        for token in SHOW_ALWAYS_TOKENS:
            if token in text:
                return True
        for token in HIDE_UI_TOKENS:
            if token in text:
                return False
        return True

    def append_log(self, kind: str, message: str) -> None:
        stamp = now_text()
        display_kind = self._log_kind_display(kind)

        if kind in ("com26", "rx_log"):
            self._handle_rx_log_line(message)

        if self.log_file is not None:
            self.log_file.write(f"{stamp} {display_kind} {message}\n")
            self.log_file_pending += 1
            if self.log_file_pending >= LOG_FILE_FLUSH_LINES:
                self.log_file.flush()
                self.log_file_pending = 0

        if not self._should_show_log(kind, message):
            return

        line = f"{stamp} {display_kind} {compact_text(message)}\n"
        self.log_text.configure(state="normal")
        self.log_text.insert("end", line, kind if kind in {"tx", "rx", "error", "status", "tx_log", "rx_log"} else "")
        self.log_line_count += 1
        if self.log_line_count > LOG_TEXT_MAX_LINES:
            self.log_text.delete("1.0", f"{LOG_TEXT_TRIM_LINES + 1}.0")
            self.log_line_count -= LOG_TEXT_TRIM_LINES
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _handle_rx_log_line(self, message: str) -> None:
        match = RX_EXEC_DONE_RE.search(message)
        if not match:
            return

        job_id = int(match.group(1))
        lines = int(match.group(2))
        x_um = int(match.group(3))
        y_um = int(match.group(4))

        self.task_status_var.set(f"执行完成：lines={lines}")
        self.enqueue_log(
            "status",
            f"EXEC_DONE job={job_id} lines={lines} x_um={x_um} y_um={y_um}",
        )
        self.after(1500, self._reset_progress_after_done)

    def _reset_progress_after_done(self) -> None:
        try:
            self.task_progress["value"] = 0
        except Exception:
            pass

    def clear_log(self) -> None:
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_line_count = 0
        self.log_text.configure(state="disabled")
        self.enqueue_log("status", "日志已清空")

    def refresh_ports(self) -> None:
        ports = self.client.available_ports()
        self.port_combo["values"] = ports
        self.tx_log_combo["values"] = ports
        self.rx_log_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_combo.current(0)
        self._select_port_by_prefix(self.tx_log_combo, self.tx_log_port_var, "COM24")
        self._select_port_by_prefix(self.rx_log_combo, self.rx_log_port_var, "COM26")
        self.enqueue_log("status", f"检测到 {len(ports)} 个串口")

    @staticmethod
    def _select_port_by_prefix(combo: ttk.Combobox, var: tk.StringVar, prefix: str) -> None:
        if var.get():
            return
        for value in combo["values"]:
            if str(value).upper().startswith(prefix.upper()):
                var.set(value)
                return

    def toggle_connection(self) -> None:
        if self.client.is_open():
            self.client.close()
            self.connect_button.configure(text="连接")
            self.enqueue_log("status", "串口已断开")
            return
        display = self.port_var.get().strip()
        if not display:
            messagebox.showwarning("串口", "请选择 TX 发射板串口")
            return
        try:
            baud = int(self.baud_var.get().strip())
            port = self.client.port_device(display)
            self._check_port_conflict(port, "TX 命令串口")
            self.client.open(port, baud)
            self.connect_button.configure(text="断开")
            self._start_default_log()
        except Exception as exc:
            self.enqueue_log("error", str(exc))
            messagebox.showerror("连接失败", str(exc))

    def toggle_tx_log_monitor(self) -> None:
        self._toggle_monitor(self.tx_log_monitor, self.tx_log_port_var, self.tx_log_button, "监听TX日志", "停止TX日志")

    def toggle_rx_log_monitor(self) -> None:
        self._toggle_monitor(self.rx_log_monitor, self.rx_log_port_var, self.rx_log_button, "监听RX日志", "停止RX日志")

    def _toggle_monitor(
        self,
        monitor: SerialLogMonitor,
        var: tk.StringVar,
        button: ttk.Button,
        start_text: str,
        stop_text: str,
    ) -> None:
        if monitor.is_open():
            monitor.close()
            button.configure(text=start_text)
            return
        display = var.get().strip()
        if not display:
            messagebox.showwarning("调试串口", "请选择要监听的调试串口")
            return
        try:
            baud = int(self.baud_var.get().strip())
            port = SleJobSerialClient.port_device(display)
            self._check_port_conflict(port, "调试日志串口")
            monitor.open(port, baud)
            button.configure(text=stop_text)
            self._start_default_log()
        except Exception as exc:
            self.enqueue_log("error", str(exc))
            messagebox.showerror("监听失败", str(exc))

    def _check_port_conflict(self, port: str, role: str) -> None:
        opened = []
        if self.client.is_open() and self.client._serial is not None:
            opened.append(("TX 命令串口", self.client._serial.port))
        if self.tx_log_monitor.is_open():
            opened.append(("TX 日志串口", self.tx_log_monitor.port))
        if self.rx_log_monitor.is_open():
            opened.append(("RX 日志串口", self.rx_log_monitor.port))
        for opened_role, opened_port in opened:
            if opened_port and opened_port.upper() == port.upper():
                raise RuntimeError(f"{role} 不能重复打开 {port}，已被 {opened_role} 使用")

    def _set_log_path(self, path: Path) -> None:
        self._close_log_file()
        path.parent.mkdir(parents=True, exist_ok=True)
        self.log_path = path
        self.log_file = path.open("a", encoding="utf-8")
        self.log_file_pending = 0

    def _close_log_file(self) -> None:
        if self.log_file is not None:
            try:
                self.log_file.flush()
                self.log_file.close()
            except OSError:
                pass
        self.log_file = None
        self.log_file_pending = 0

    def _start_default_log(self) -> None:
        if self.log_path is None:
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            self._set_log_path(Path(__file__).resolve().parent / "logs" / f"session_{stamp}.log")
            self.enqueue_log("status", f"日志文件: {self.log_path}")

    def save_log_as(self) -> None:
        path = filedialog.asksaveasfilename(
            title="保存日志",
            defaultextension=".log",
            filetypes=[("Log files", "*.log"), ("Text files", "*.txt"), ("All files", "*.*")],
        )
        if path:
            self._set_log_path(Path(path))
            self.enqueue_log("status", f"日志文件: {self.log_path}")

    def load_file(self) -> None:
        path = filedialog.askopenfilename(
            title="打开 G-code",
            filetypes=[("G-code", "*.gcode *.nc *.txt"), ("All files", "*.*")],
        )
        if not path:
            return
        data = Path(path).read_bytes()
        text = data.decode("utf-8", errors="replace")
        self.loaded_file = Path(path)
        self.file_var.set(str(self.loaded_file))
        self.gcode_text.delete("1.0", "end")
        self.gcode_text.insert("1.0", text)
        self.enqueue_log("status", f"已加载: {path} ({len(data)} bytes)")

    def _job_id(self) -> int:
        value = int(self.job_id_var.get().strip())
        if value <= 0:
            raise ValueError("Job ID 必须大于 0")
        return value

    def _timeout(self) -> float:
        return max(1.0, float(self.timeout_var.get().strip() or UPLOAD_TIMEOUT_DEFAULT))

    def _gcode_bytes(self) -> bytes:
        text = normalize_gcode_text(self.gcode_text.get("1.0", "end-1c"))
        return text.encode("utf-8")

    def _run_worker(self, target: Callable[[], None]) -> None:
        if self.worker_thread is not None and self.worker_thread.is_alive():
            self.enqueue_log("error", "任务进行中，请等待完成")
            return
        self.worker_thread = threading.Thread(target=target, daemon=True)
        self.worker_thread.start()

    def _on_mode_change(self) -> None:
        if self.exec_mode_var.get() == "preroll":
            self.preroll_entry.configure(state="normal")
        else:
            self.preroll_entry.configure(state="disabled")

    def _get_preroll_bytes(self) -> int:
        if self.exec_mode_var.get() == "normal":
            return 0
        text = self.preroll_bytes_var.get().strip()
        if not text:
            raise ValueError("预缓冲大小不能为空")
        value = int(text)
        if value <= 0:
            raise ValueError("预缓冲大小必须大于 0")
        return value

    def upload_job(self) -> None:
        self._run_worker(lambda: self._upload_worker(start_after=False))

    def upload_and_start(self) -> None:
        self._run_worker(lambda: self._upload_worker(start_after=True))

    def _upload_worker(self, *, start_after: bool) -> None:
        try:
            job_id = self._job_id()
            timeout = self._timeout()
            gcode = self._gcode_bytes()
            crc = crc16_ccitt(gcode)
            total = len(gcode)
            preroll = self._get_preroll_bytes()
            mode_text = "preroll" if preroll > 0 else "normal"
            self.enqueue_task_status("正在建立任务", 0)
            self.enqueue_log("status", f"上传配置: mode={mode_text} "
                             f"chunk={JOB_DATA_CHUNK_SIZE}B "
                             f"serial_burst={SERIAL_WRITE_BURST_SIZE}B "
                             f"gap={int(SERIAL_WRITE_BURST_GAP_S * 1000)}ms "
                             f"baud={BAUD_DEFAULT}"
                             + (f" preroll={preroll}B" if preroll > 0 else ""))
            self.enqueue_progress(f"上传中 {total} bytes crc=0x{crc:04x}")
            self.enqueue_log("status", f"开始上传 job={job_id} size={total} crc=0x{crc:04x}")
            self.client.upload_job(
                job_id, gcode, timeout,
                wait_ready=True,
                progress_cb=self.enqueue_progress,
                status_cb=self.enqueue_task_status,
                total=total,
                preroll_bytes=preroll,
            )
            self.enqueue_task_status("下发完成，可执行", 100)
            self.enqueue_progress(f"上传完成 job={job_id}")
            if start_after and preroll == 0:
                self.enqueue_log("status", "RX 已确认 JOB_READY，发送 EXEC_START")
                try:
                    self._exec_start_worker()
                except Exception as exec_exc:
                    self.enqueue_log("error", f"EXEC_START 失败: {exec_exc}")
                    self.enqueue_task_status(f"执行失败: {exec_exc}")
        except Exception as exc:
            self.enqueue_progress("失败")
            self.enqueue_task_status(f"上传失败: {exc}")
            self.enqueue_log("error", str(exc))

    def exec_start(self) -> None:
        self._run_worker(self._exec_start_worker)

    def _exec_start_worker(self) -> None:
        try:
            self.enqueue_task_status("执行中", -1)
            job_id = self._job_id()
            result = self.client.send_control(
                f"@EXEC_START {job_id}",
                "@ACK type=16",
                self._timeout(),
            )
            self.enqueue_progress(f"执行启动 job={job_id}")
            self.enqueue_log("status", f"EXEC_START ACK {result.elapsed_ms} ms")
            self.enqueue_task_status("执行完成", 100)
        except Exception as exc:
            self.enqueue_log("error", str(exc))
            self.enqueue_task_status(f"执行失败: {exc}")

    def exec_stop(self) -> None:
        self._run_worker(lambda: self._control_worker("@EXEC_STOP", "@ACK type=19", "已停止"))

    def abort_job(self) -> None:
        self._run_worker(lambda: self._control_worker("@ABORT", "@ACK type=4", "已放弃任务"))

    def query_status(self) -> None:
        self._run_worker(self._status_worker)

    def _status_worker(self) -> None:
        try:
            result = self.client.transact_status(CMD_TIMEOUT_DEFAULT)
            self.enqueue_log("status", f"STATUS {result.elapsed_ms} ms: {result.line}")
        except Exception as exc:
            self.enqueue_log("error", str(exc))

    def _control_worker(self, command: str, expect: str, done_text: str) -> None:
        try:
            result = self.client.send_control(command, expect, CMD_TIMEOUT_DEFAULT)
            self.enqueue_progress(done_text)
            self.enqueue_log("status", f"{command} ACK {result.elapsed_ms} ms: {result.line}")
        except Exception as exc:
            self.enqueue_log("error", str(exc))

    def on_close(self) -> None:
        self.client.close()
        self.tx_log_monitor.close()
        self.rx_log_monitor.close()
        self._close_log_file()
        self.destroy()


def main() -> None:
    app = SleJobHostApp()
    app.mainloop()


if __name__ == "__main__":
    main()
