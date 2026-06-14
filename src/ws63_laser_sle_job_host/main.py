from __future__ import annotations

import queue
import re
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Iterable, Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - runtime dependency
    raise SystemExit("缺少 pyserial，请先运行: pip install pyserial") from exc

import tkinter as tk
from tkinter import filedialog, messagebox, ttk


BAUD_DEFAULT = 115200
LINE_TIMEOUT_DEFAULT = 8.0
STATUS_TIMEOUT_DEFAULT = 2.0


def now_text() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def normalize_gcode_lines(text: str) -> list[str]:
    lines: list[str] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith(";") or line.startswith("("):
            continue
        if ";" in line:
            line = line.split(";", 1)[0].rstrip()
        if line:
            lines.append(line)
    return lines


def compact_line(line: str, limit: int = 180) -> str:
    clean = line.replace("\r", "\\r").replace("\n", "\\n")
    if len(clean) <= limit:
        return clean
    return clean[: limit - 3] + "..."


@dataclass
class CommandResult:
    line: str
    ok: bool
    response: str
    elapsed_ms: int


class SerialGcodeClient:
    def __init__(self, on_log: Callable[[str, str], None]) -> None:
        self._on_log = on_log
        self._serial: Optional[serial.Serial] = None
        self._reader: Optional[threading.Thread] = None
        self._running = threading.Event()
        self._lines: "queue.Queue[str]" = queue.Queue()
        self._lock = threading.Lock()
        self._command_lock = threading.Lock()
        self._line_seq = 0

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
        self._serial = serial.Serial(port, baudrate=baud, timeout=0.05, write_timeout=1.0)
        self._serial.reset_input_buffer()
        self._serial.reset_output_buffer()
        self._running.set()
        self._reader = threading.Thread(target=self._read_loop, name="serial-reader", daemon=True)
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

    def _drain_response_queue(self) -> None:
        while True:
            try:
                self._lines.get_nowait()
            except queue.Empty:
                return

    def write_raw(self, payload: bytes, label: str) -> None:
        if not self.is_open() or self._serial is None:
            raise RuntimeError("串口未连接")
        with self._lock:
            self._serial.write(payload)
            self._serial.flush()
        self._on_log("tx", label)

    def send_realtime(self, char: bytes, label: str) -> None:
        self.write_raw(char, label)

    def send_line(self, line: str, timeout: float = LINE_TIMEOUT_DEFAULT, expect_ok: bool = True) -> CommandResult:
        if not self.is_open() or self._serial is None:
            raise RuntimeError("串口未连接")
        clean = line.strip()
        if not clean:
            raise RuntimeError("空命令")

        with self._command_lock:
            self._line_seq += 1
            seq = self._line_seq
            self._drain_response_queue()
            started = time.monotonic()
            with self._lock:
                self._serial.write((clean + "\n").encode("utf-8"))
                self._serial.flush()
            self._on_log("tx", f"#{seq} {clean}")

            if not expect_ok:
                return self._wait_for_status(clean, started, timeout)
            return self._wait_for_ok(clean, started, timeout)

    def _wait_for_ok(self, line: str, started: float, timeout: float) -> CommandResult:
        deadline = started + timeout
        last_response = ""
        while time.monotonic() < deadline:
            try:
                response = self._lines.get(timeout=0.05)
            except queue.Empty:
                continue
            last_response = response
            lower = response.lower()
            if lower == "ok" or lower.startswith("ok "):
                elapsed_ms = int((time.monotonic() - started) * 1000)
                self._on_log("status", f"OK {elapsed_ms} ms <- {line}")
                return CommandResult(line, True, response, elapsed_ms)
            if lower.startswith("error"):
                elapsed_ms = int((time.monotonic() - started) * 1000)
                raise RuntimeError(f"{line} 返回 {response}，耗时 {elapsed_ms} ms")
        elapsed_ms = int((time.monotonic() - started) * 1000)
        raise TimeoutError(f"{line} 等待 ok 超时，耗时 {elapsed_ms} ms，最后响应: {last_response or '无'}")

    def _wait_for_status(self, line: str, started: float, timeout: float) -> CommandResult:
        deadline = started + timeout
        last_response = ""
        while time.monotonic() < deadline:
            try:
                response = self._lines.get(timeout=0.05)
            except queue.Empty:
                continue
            last_response = response
            if response.startswith("<") and response.endswith(">"):
                elapsed_ms = int((time.monotonic() - started) * 1000)
                self._on_log("status", f"STATUS {elapsed_ms} ms <- {response}")
                return CommandResult(line, True, response, elapsed_ms)
            if response.lower().startswith("error"):
                elapsed_ms = int((time.monotonic() - started) * 1000)
                raise RuntimeError(f"{line} 返回 {response}，耗时 {elapsed_ms} ms")
        elapsed_ms = int((time.monotonic() - started) * 1000)
        raise TimeoutError(f"{line} 等待状态超时，耗时 {elapsed_ms} ms，最后响应: {last_response or '无'}")


class GcodeSenderApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("WS63 SLE Bridge G-code Sender")
        self.geometry("1180x760")
        self.minsize(980, 640)

        self.log_queue: "queue.Queue[tuple[str, str]]" = queue.Queue()
        self.client = SerialGcodeClient(self.enqueue_log)
        self.sender_thread: Optional[threading.Thread] = None
        self.stop_requested = threading.Event()
        self.log_path: Optional[Path] = None

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

        ttk.Label(top, text="串口").grid(row=0, column=0, padx=(0, 6))
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

        main = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        main.grid(row=1, column=0, sticky="nsew", padx=8, pady=(0, 8))

        left = ttk.Frame(main, padding=8)
        right = ttk.Frame(main, padding=8)
        main.add(left, weight=3)
        main.add(right, weight=2)

        left.rowconfigure(1, weight=1)
        left.columnconfigure(0, weight=1)
        ttk.Label(left, text="G-code").grid(row=0, column=0, sticky="w")
        self.gcode_text = tk.Text(left, height=18, wrap="none", undo=True)
        self.gcode_text.grid(row=1, column=0, sticky="nsew", pady=(4, 8))
        self.gcode_text.insert(
            "1.0",
            "$X\nG90\nM3 S50 F1000\nG1 X12 Y5 F1000\nG1 X0 Y0 F1000\nM5\n$D\n",
        )

        controls = ttk.Frame(left)
        controls.grid(row=2, column=0, sticky="ew")
        controls.columnconfigure(8, weight=1)
        ttk.Button(controls, text="打开文件", command=self.load_file).grid(row=0, column=0, padx=(0, 6))
        ttk.Button(controls, text="发送全部", command=self.send_all).grid(row=0, column=1, padx=(0, 6))
        ttk.Button(controls, text="发送选中", command=self.send_selection).grid(row=0, column=2, padx=(0, 6))
        ttk.Button(controls, text="停止", command=self.stop_job).grid(row=0, column=3, padx=(0, 12))

        ttk.Label(controls, text="行超时(s)").grid(row=0, column=4, padx=(0, 4))
        self.timeout_var = tk.StringVar(value=str(LINE_TIMEOUT_DEFAULT))
        ttk.Entry(controls, textvariable=self.timeout_var, width=6).grid(row=0, column=5, padx=(0, 8))
        ttk.Label(controls, text="行间隔(ms)").grid(row=0, column=6, padx=(0, 4))
        self.delay_var = tk.StringVar(value="0")
        ttk.Entry(controls, textvariable=self.delay_var, width=6).grid(row=0, column=7, padx=(0, 8))
        self.progress_var = tk.StringVar(value="待发送")
        ttk.Label(controls, textvariable=self.progress_var).grid(row=0, column=8, sticky="e")

        quick = ttk.Frame(left)
        quick.grid(row=3, column=0, sticky="ew", pady=(8, 0))
        for idx, (label, command) in enumerate(
            [
                ("?", self.query_status),
                ("$D", lambda: self.send_quick("$D")),
                ("$I", lambda: self.send_quick("$I")),
                ("$$", lambda: self.send_quick("$$")),
                ("M5", lambda: self.send_quick("M5")),
                ("Ctrl-X", self.soft_reset),
            ]
        ):
            ttk.Button(quick, text=label, command=command).grid(row=0, column=idx, padx=(0, 6))

        right.rowconfigure(1, weight=1)
        right.columnconfigure(0, weight=1)
        ttk.Label(right, text="通信日志").grid(row=0, column=0, sticky="w")
        self.log_text = tk.Text(right, height=20, wrap="word", state="disabled")
        self.log_text.grid(row=1, column=0, sticky="nsew", pady=(4, 0))
        self.log_text.tag_configure("tx", foreground="#0B6E4F")
        self.log_text.tag_configure("rx", foreground="#1B4D89")
        self.log_text.tag_configure("error", foreground="#B00020")
        self.log_text.tag_configure("status", foreground="#7A4E00")

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
        line = f"{now_text()} {kind.upper():>6} {compact_line(message)}\n"
        self.log_text.configure(state="normal")
        self.log_text.insert("end", line, kind if kind in {"tx", "rx", "error", "status"} else "")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")
        if self.log_path is not None:
            self.log_path.parent.mkdir(parents=True, exist_ok=True)
            with self.log_path.open("a", encoding="utf-8") as f:
                f.write(line)

    def refresh_ports(self) -> None:
        ports = self.client.available_ports()
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_combo.current(0)
        self.enqueue_log("status", f"检测到 {len(ports)} 个串口")

    def toggle_connection(self) -> None:
        if self.client.is_open():
            self.client.close()
            self.connect_button.configure(text="连接")
            self.enqueue_log("status", "串口已断开")
            return
        display = self.port_var.get().strip()
        if not display:
            messagebox.showwarning("串口", "请选择串口")
            return
        try:
            baud = int(self.baud_var.get().strip())
            self.client.open(self.client.port_device(display), baud)
            self.connect_button.configure(text="断开")
            self._start_default_log()
        except Exception as exc:
            self.enqueue_log("error", str(exc))
            messagebox.showerror("连接失败", str(exc))

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
        text = Path(path).read_text(encoding="utf-8", errors="replace")
        self.gcode_text.delete("1.0", "end")
        self.gcode_text.insert("1.0", text)
        self.enqueue_log("status", f"已加载: {path}")

    def _get_selected_or_all_text(self, selected_only: bool) -> str:
        if selected_only:
            try:
                return self.gcode_text.get("sel.first", "sel.last")
            except tk.TclError:
                return ""
        return self.gcode_text.get("1.0", "end")

    def send_all(self) -> None:
        self._send_text(self._get_selected_or_all_text(False))

    def send_selection(self) -> None:
        text = self._get_selected_or_all_text(True)
        if not text.strip():
            messagebox.showwarning("发送选中", "请先选中要发送的 G-code")
            return
        self._send_text(text)

    def _send_text(self, text: str) -> None:
        if self.sender_thread is not None and self.sender_thread.is_alive():
            messagebox.showwarning("发送中", "当前已有任务在发送")
            return
        lines = normalize_gcode_lines(text)
        if not lines:
            messagebox.showwarning("G-code", "没有可发送的有效行")
            return
        self.stop_requested.clear()
        self.sender_thread = threading.Thread(target=self._send_worker, args=(lines,), daemon=True)
        self.sender_thread.start()

    def _send_worker(self, lines: list[str]) -> None:
        try:
            timeout = float(self.timeout_var.get().strip() or LINE_TIMEOUT_DEFAULT)
        except ValueError:
            timeout = LINE_TIMEOUT_DEFAULT
        try:
            delay_ms = max(0, int(float(self.delay_var.get().strip() or "0")))
        except ValueError:
            delay_ms = 0

        total = len(lines)
        self.enqueue_log("status", f"开始逐行发送 {total} 行，timeout={timeout}s delay={delay_ms}ms")
        for index, line in enumerate(lines, start=1):
            if self.stop_requested.is_set():
                self.enqueue_log("status", "发送任务已停止")
                return
            try:
                self.client.send_line(line, timeout=timeout, expect_ok=True)
            except Exception as exc:
                self.enqueue_log("error", str(exc))
                self._diagnose_after_error()
                return
            self.after(0, self.progress_var.set, f"{index}/{total}")
            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)
        self.after(0, self.progress_var.set, f"完成 {total}/{total}")
        self.enqueue_log("status", "发送完成")

    def _diagnose_after_error(self) -> None:
        for command, expect_ok, timeout in [("?", False, STATUS_TIMEOUT_DEFAULT), ("$D", True, LINE_TIMEOUT_DEFAULT)]:
            try:
                self.client.send_line(command, timeout=timeout, expect_ok=expect_ok)
            except Exception as exc:
                self.enqueue_log("error", f"诊断 {command} 失败: {exc}")

    def stop_job(self) -> None:
        self.stop_requested.set()
        self.enqueue_log("status", "请求停止发送")

    def send_quick(self, line: str) -> None:
        if self.sender_thread is not None and self.sender_thread.is_alive():
            messagebox.showwarning("发送中", "请先停止当前任务")
            return

        def worker() -> None:
            try:
                self.client.send_line(line, timeout=float(self.timeout_var.get() or LINE_TIMEOUT_DEFAULT), expect_ok=True)
            except Exception as exc:
                self.enqueue_log("error", str(exc))

        threading.Thread(target=worker, daemon=True).start()

    def query_status(self) -> None:
        def worker() -> None:
            try:
                self.client.send_line("?", timeout=STATUS_TIMEOUT_DEFAULT, expect_ok=False)
            except Exception as exc:
                self.enqueue_log("error", str(exc))

        threading.Thread(target=worker, daemon=True).start()

    def soft_reset(self) -> None:
        try:
            self.client.send_realtime(b"\x18", "Ctrl-X")
        except Exception as exc:
            self.enqueue_log("error", str(exc))

    def on_close(self) -> None:
        self.stop_requested.set()
        self.client.close()
        self.destroy()


def main() -> None:
    app = GcodeSenderApp()
    app.mainloop()


if __name__ == "__main__":
    main()
