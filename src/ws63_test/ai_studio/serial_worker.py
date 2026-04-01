"""
串口通信与任务调度模块。

要求：
1. 独立 QThread 处理串口；
2. 一条 G-Code 一条 G-Code 地发；
3. 每发一条必须等待下位机返回 ok；
4. 支持急停，急停时立即清空队列并下发 M5 和 !。
"""

from __future__ import annotations

import queue
import time
from typing import List, Optional

import serial
from serial import Serial
from serial.tools import list_ports

from .qt_compat import QtCore, Signal


class SerialWorkerThread(QtCore.QThread):
    """串口发送线程。"""

    log_signal = Signal(str)
    status_signal = Signal(str)
    progress_signal = Signal(int)
    ports_signal = Signal(list)
    finished_signal = Signal(bool, str)

    def __init__(self, parent: Optional[QtCore.QObject] = None) -> None:
        super().__init__(parent)
        self._serial: Optional[Serial] = None
        self._command_queue: "queue.Queue[List[str]]" = queue.Queue()
        self._running = True
        self._connected = False
        self._emergency_stop = False
        self._wait_ok_timeout = 8.0

    @staticmethod
    def list_available_ports() -> List[str]:
        return [port.device for port in list_ports.comports()]

    def connect_port(self, port_name: str, baud_rate: int = 115200) -> None:
        if self._connected:
            self.disconnect_port()
        try:
            self._serial = serial.Serial(port_name, baudrate=baud_rate, timeout=0.1)
            self._serial.reset_input_buffer()
            self._serial.reset_output_buffer()
            self._connected = True
            self.status_signal.emit(f"串口已连接: {port_name} @ {baud_rate}")
        except serial.SerialException as exc:
            self._serial = None
            self._connected = False
            raise RuntimeError(f"串口连接失败: {exc}") from exc

    def disconnect_port(self) -> None:
        if self._serial is not None:
            try:
                self._serial.close()
            except serial.SerialException:
                pass
        self._serial = None
        self._connected = False
        self.status_signal.emit("串口已断开")

    def enqueue_gcode(self, gcode_lines: List[str]) -> None:
        if not gcode_lines:
            raise RuntimeError("G-Code 队列为空，无法发送")
        self._command_queue.put(gcode_lines)
        self.status_signal.emit(f"已加入发送队列，共 {len(gcode_lines)} 行")

    def emergency_stop(self) -> None:
        self._emergency_stop = True
        # 清空待发送队列，防止急停后继续执行旧任务。
        while not self._command_queue.empty():
            try:
                self._command_queue.get_nowait()
            except queue.Empty:
                break
        self.status_signal.emit("已触发急停，正在停止任务")

    def stop(self) -> None:
        self._running = False
        self.emergency_stop()
        self.wait(1500)
        self.disconnect_port()

    def _write_line(self, line: str) -> None:
        if self._serial is None or not self._connected:
            raise RuntimeError("串口未连接，无法发送")
        payload = (line.rstrip() + "\n").encode("utf-8")
        self._serial.write(payload)
        self._serial.flush()
        self.log_signal.emit(f"[TX] {line}")

    def _wait_for_ok(self) -> None:
        if self._serial is None:
            raise RuntimeError("串口未连接")

        deadline = time.monotonic() + self._wait_ok_timeout
        buffer = ""
        while time.monotonic() < deadline:
            if self._emergency_stop:
                raise RuntimeError("任务已被急停")
            chunk = self._serial.read(self._serial.in_waiting or 1)
            if not chunk:
                time.sleep(0.01)
                continue

            try:
                text = chunk.decode("utf-8", errors="replace")
            except Exception:
                text = repr(chunk)
            buffer += text

            for raw_line in buffer.replace("\r", "\n").split("\n"):
                line = raw_line.strip()
                if not line:
                    continue
                self.log_signal.emit(f"[RX] {line}")
                if "ok" in line.lower():
                    return
                if line.lower().startswith("error"):
                    raise RuntimeError(f"下位机返回错误: {line}")

            if "\n" in buffer or "\r" in buffer:
                buffer = ""

        raise RuntimeError("等待下位机 ok 超时")

    def _send_emergency_commands(self) -> None:
        if self._serial is None or not self._connected:
            return
        try:
            self._write_line("M5")
            # Grbl 风格实时急停指令，不走换行协议。
            self._serial.write(b"!")
            self._serial.flush()
            self.log_signal.emit("[TX] !")
        except Exception as exc:  # pragma: no cover - 硬件环境相关
            self.log_signal.emit(f"[ERR] 急停指令发送失败: {exc}")

    def _send_job(self, gcode_lines: List[str]) -> None:
        total = len(gcode_lines)
        for index, line in enumerate(gcode_lines, start=1):
            if self._emergency_stop:
                self._send_emergency_commands()
                raise RuntimeError("任务被急停终止")

            self._write_line(line)
            self._wait_for_ok()
            progress = int(index * 100 / max(total, 1))
            self.progress_signal.emit(progress)

    def run(self) -> None:
        while self._running:
            try:
                ports = self.list_available_ports()
                self.ports_signal.emit(ports)
                try:
                    job = self._command_queue.get(timeout=0.2)
                except queue.Empty:
                    continue

                if not self._connected or self._serial is None:
                    self.finished_signal.emit(False, "串口未连接，无法执行任务")
                    continue

                self._emergency_stop = False
                self.progress_signal.emit(0)
                self.status_signal.emit("开始发送 G-Code")
                self._send_job(job)
                self.progress_signal.emit(100)
                self.finished_signal.emit(True, "任务执行完成")
            except Exception as exc:  # pragma: no cover - 运行时相关
                self.finished_signal.emit(False, str(exc))
                if self._emergency_stop:
                    self._send_emergency_commands()
                    self._emergency_stop = False

