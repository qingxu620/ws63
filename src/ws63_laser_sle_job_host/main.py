from __future__ import annotations

import queue
import re
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Optional

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

    def write_bytes(self, payload: bytes, label: str) -> None:
        if not self.is_open() or self._serial is None:
            raise RuntimeError("串口未连接")
        with self._write_lock:
            self._serial.write(payload)
            self._serial.flush()
        self._on_log("tx", label)

    def send_line(self, line: str) -> None:
        clean = line.strip()
        if not clean:
            raise RuntimeError("空命令")
        self.write_bytes((clean + "\n").encode("ascii"), clean)

    def wait_for(self, pattern: str, timeout: float, *, regex: bool = False) -> WaitResult:
        started = time.monotonic()
        deadline = started + timeout
        compiled = re.compile(pattern) if regex else None
        last_line = ""
        while time.monotonic() < deadline:
            try:
                line = self._lines.get(timeout=0.05)
            except queue.Empty:
                continue
            last_line = line
            matched = bool(compiled.search(line)) if compiled is not None else pattern in line
            if matched:
                return WaitResult(line=line, elapsed_ms=int((time.monotonic() - started) * 1000))
            if line.startswith("@ERR") or line.startswith("@NACK"):
                raise RuntimeError(f"收到错误响应: {line}")
        elapsed_ms = int((time.monotonic() - started) * 1000)
        raise TimeoutError(f"等待 {pattern} 超时，耗时 {elapsed_ms} ms，最后响应: {last_line or '无'}")

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

    def upload_job(self, job_id: int, gcode: bytes, timeout: float) -> None:
        if not gcode:
            raise RuntimeError("G-code 内容为空")
        if len(gcode) > 64 * 1024:
            raise RuntimeError("第一版 RX RAM 缓存默认 64KB，请先缩小 G-code 文件")

        crc = crc16_ccitt(gcode)
        with self._command_lock:
            self._drain_lines()
            self.send_line(f"@BEGIN {job_id} {len(gcode)} {crc:04x}")
            ready = self.wait_for(f"@DATA_READY job={job_id}", timeout)
            self._on_log("status", f"DATA_READY {ready.elapsed_ms} ms")
            self.write_bytes(gcode, f"<raw gcode> {len(gcode)} bytes crc=0x{crc:04x}")
            done = self.wait_for(f"@JOB_READY job={job_id}", timeout)
            self._on_log("status", f"JOB_READY {done.elapsed_ms} ms")


class SerialLogMonitor:
    def __init__(self, name: str, on_log: Callable[[str, str], None]) -> None:
        self._name = name
        self._on_log = on_log
        self._serial: Optional[serial.Serial] = None
        self._reader: Optional[threading.Thread] = None
        self._running = threading.Event()

    def is_open(self) -> bool:
        return self._serial is not None and self._serial.is_open

    def open(self, port: str, baud: int) -> None:
        self.close()
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
        self.client = SleJobSerialClient(self.enqueue_log)
        self.tx_log_monitor = SerialLogMonitor("COM24", self.enqueue_log)
        self.rx_log_monitor = SerialLogMonitor("COM26", self.enqueue_log)
        self.worker_thread: Optional[threading.Thread] = None
        self.log_path: Optional[Path] = None
        self.loaded_file: Optional[Path] = None

        self._build_ui()
        self.refresh_ports()
        self.after(50, self._pump_logs)
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
        self.tx_log_button = ttk.Button(top, text="监听COM24", command=self.toggle_tx_log_monitor)
        self.tx_log_button.grid(row=1, column=2, padx=(0, 12), pady=(6, 0))

        ttk.Label(top, text="RX日志").grid(row=1, column=3, padx=(0, 6), pady=(6, 0))
        self.rx_log_port_var = tk.StringVar()
        self.rx_log_combo = ttk.Combobox(top, textvariable=self.rx_log_port_var, width=34, state="readonly")
        self.rx_log_combo.grid(row=1, column=4, sticky="ew", padx=(0, 6), pady=(6, 0))
        self.rx_log_button = ttk.Button(top, text="监听COM26", command=self.toggle_rx_log_monitor)
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

        quick = ttk.Frame(left)
        quick.grid(row=3, column=0, sticky="ew", pady=(8, 0))
        ttk.Button(quick, text="@STATUS", command=self.query_status).grid(row=0, column=0, padx=(0, 6))
        ttk.Button(quick, text="@EXEC_START", command=self.exec_start).grid(row=0, column=1, padx=(0, 6))
        ttk.Button(quick, text="@EXEC_STOP", command=self.exec_stop).grid(row=0, column=2, padx=(0, 6))
        ttk.Button(quick, text="@ABORT", command=self.abort_job).grid(row=0, column=3, padx=(0, 6))

        right.rowconfigure(1, weight=1)
        right.columnconfigure(0, weight=1)
        ttk.Label(right, text="通信日志").grid(row=0, column=0, sticky="w")
        self.log_text = tk.Text(right, height=20, wrap="word", state="disabled")
        self.log_text.grid(row=1, column=0, sticky="nsew", pady=(4, 0))
        self.log_text.tag_configure("tx", foreground="#0B6E4F")
        self.log_text.tag_configure("rx", foreground="#1B4D89")
        self.log_text.tag_configure("error", foreground="#B00020")
        self.log_text.tag_configure("status", foreground="#7A4E00")
        self.log_text.tag_configure("com24", foreground="#6A1B9A")
        self.log_text.tag_configure("com26", foreground="#00695C")

    def enqueue_log(self, kind: str, message: str) -> None:
        self.log_queue.put((kind, message))

    def _pump_logs(self) -> None:
        while True:
            try:
                kind, message = self.log_queue.get_nowait()
            except queue.Empty:
                break
            self.append_log(kind, message)
        self.after(50, self._pump_logs)

    def append_log(self, kind: str, message: str) -> None:
        line = f"{now_text()} {kind.upper():>6} {compact_text(message)}\n"
        self.log_text.configure(state="normal")
        self.log_text.insert("end", line, kind if kind in {"tx", "rx", "error", "status", "com24", "com26"} else "")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")
        if self.log_path is not None:
            self.log_path.parent.mkdir(parents=True, exist_ok=True)
            with self.log_path.open("a", encoding="utf-8") as f:
                f.write(line)

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
            self.client.open(self.client.port_device(display), baud)
            self.connect_button.configure(text="断开")
            self._start_default_log()
        except Exception as exc:
            self.enqueue_log("error", str(exc))
            messagebox.showerror("连接失败", str(exc))

    def toggle_tx_log_monitor(self) -> None:
        self._toggle_monitor(self.tx_log_monitor, self.tx_log_port_var, self.tx_log_button, "监听COM24", "停止COM24")

    def toggle_rx_log_monitor(self) -> None:
        self._toggle_monitor(self.rx_log_monitor, self.rx_log_port_var, self.rx_log_button, "监听COM26", "停止COM26")

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
            monitor.open(SleJobSerialClient.port_device(display), baud)
            button.configure(text=stop_text)
            self._start_default_log()
        except Exception as exc:
            self.enqueue_log("error", str(exc))
            messagebox.showerror("监听失败", str(exc))

    def _start_default_log(self) -> None:
        if self.log_path is None:
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            self.log_path = Path(__file__).resolve().parent / "logs" / f"session_{stamp}.log"
            self.enqueue_log("status", f"日志文件: {self.log_path}")

    def save_log_as(self) -> None:
        path = filedialog.asksaveasfilename(
            title="保存日志",
            defaultextension=".log",
            filetypes=[("Log files", "*.log"), ("Text files", "*.txt"), ("All files", "*.*")],
        )
        if path:
            self.log_path = Path(path)
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
            messagebox.showwarning("任务进行中", "当前已有上位机任务在执行")
            return
        self.worker_thread = threading.Thread(target=target, daemon=True)
        self.worker_thread.start()

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
            self.after(0, self.progress_var.set, f"上传中 {len(gcode)} bytes crc=0x{crc:04x}")
            self.enqueue_log("status", f"开始上传 job={job_id} size={len(gcode)} crc=0x{crc:04x}")
            self.client.upload_job(job_id, gcode, timeout)
            self.after(0, self.progress_var.set, f"JOB_READY job={job_id}")
            if start_after:
                self._exec_start_worker()
        except Exception as exc:
            self.after(0, self.progress_var.set, "失败")
            self.enqueue_log("error", str(exc))

    def exec_start(self) -> None:
        self._run_worker(self._exec_start_worker)

    def _exec_start_worker(self) -> None:
        try:
            job_id = self._job_id()
            result = self.client.send_control(
                f"@EXEC_START {job_id}",
                "status=0",
                self._timeout(),
            )
            self.after(0, self.progress_var.set, f"执行启动 job={job_id}")
            self.enqueue_log("status", f"EXEC_START ACK {result.elapsed_ms} ms")
        except Exception as exc:
            self.enqueue_log("error", str(exc))

    def exec_stop(self) -> None:
        self._run_worker(lambda: self._control_worker("@EXEC_STOP", "status=0", "已停止"))

    def abort_job(self) -> None:
        self._run_worker(lambda: self._control_worker("@ABORT", "status=0", "已放弃任务"))

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
            self.after(0, self.progress_var.set, done_text)
            self.enqueue_log("status", f"{command} ACK {result.elapsed_ms} ms: {result.line}")
        except Exception as exc:
            self.enqueue_log("error", str(exc))

    def on_close(self) -> None:
        self.client.close()
        self.tx_log_monitor.close()
        self.rx_log_monitor.close()
        self.destroy()


def main() -> None:
    app = SleJobHostApp()
    app.mainloop()


if __name__ == "__main__":
    main()
