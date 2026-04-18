"""
设备通信与任务调度模块。

比赛版 AI 上位机支持两种下发链路：
1. 实物串口 UART；
2. 已打通的 WiFi TCP 通路。

两种链路都保持同一套调度语义：
- 独立 QThread 处理发送任务；
- 一条 G-Code 一条 G-Code 地发；
- 每发一条必须等待下位机返回 ok；
- 支持急停，串口模式发送 ``M5 + !``，WiFi 模式发送 ``M5``。
"""

from __future__ import annotations

import queue
import sys
import time
from pathlib import Path
from typing import List, Optional

import serial
from serial import Serial
from serial.tools import list_ports

from .qt_compat import QtCore, Signal

_TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

from wifi_client import ConnectionError_, TimeoutError_, WifiGcodeClient  # type: ignore


class SerialWorkerThread(QtCore.QThread):
    """统一设备发送线程，兼容 UART 与 WiFi TCP。"""

    log_signal = Signal(str)
    status_signal = Signal(str)
    progress_signal = Signal(int)
    ports_signal = Signal(list)
    finished_signal = Signal(bool, str)

    def __init__(self, parent: Optional[QtCore.QObject] = None) -> None:
        super().__init__(parent)
        self._serial: Optional[Serial] = None
        self._wifi_client: Optional[WifiGcodeClient] = None
        self._command_queue: "queue.Queue[List[str]]" = queue.Queue()
        self._running = True
        self._connected = False
        self._emergency_stop = False
        self._wait_ok_timeout = 8.0
        self._transport_mode = "serial"
        self._connection_desc = ""

    @staticmethod
    def list_available_ports() -> List[str]:
        return [port.device for port in list_ports.comports()]

    @staticmethod
    def _normalize_gcode_lines(gcode_lines: List[str]) -> List[str]:
        """去除空行与注释，保持串口和 WiFi 两条链路行为一致。"""
        normalized: List[str] = []
        for raw in gcode_lines:
            line = raw.strip()
            if not line or line.startswith(";") or line.startswith("("):
                continue
            if ";" in line:
                line = line[: line.index(";")].rstrip()
            if line:
                normalized.append(line)
        return normalized

    def set_transport_mode(self, mode: str) -> None:
        if mode not in ("serial", "wifi"):
            raise ValueError(f"unsupported transport mode: {mode}")
        self._transport_mode = mode

    def transport_mode(self) -> str:
        return self._transport_mode

    def connection_description(self) -> str:
        return self._connection_desc

    def connect_port(self, port_name: str, baud_rate: int = 115200) -> None:
        if self._connected:
            self.disconnect_transport()
        try:
            self._serial = serial.Serial(port_name, baudrate=baud_rate, timeout=0.1)
            self._serial.reset_input_buffer()
            self._serial.reset_output_buffer()
            self._wifi_client = None
            self._transport_mode = "serial"
            self._connected = True
            self._connection_desc = f"{port_name} @ {baud_rate}"
            self.status_signal.emit(f"串口已连接: {port_name} @ {baud_rate}")
        except serial.SerialException as exc:
            self._serial = None
            self._connected = False
            self._connection_desc = ""
            raise RuntimeError(f"串口连接失败: {exc}") from exc

    def _on_wifi_log_line(self, line: str) -> None:
        if line.startswith("> "):
            self.log_signal.emit(f"[TX] {line[2:]}")
            return
        self.log_signal.emit(f"[RX] {line}")

    def connect_wifi(self, host: str, port: int = 5000, timeout: float = 5.0) -> None:
        if self._connected:
            self.disconnect_transport()

        client = WifiGcodeClient()
        client.set_on_line(self._on_wifi_log_line)
        try:
            client.connect(host, port, timeout=timeout)
            client.read_intro(timeout=timeout)
        except (ConnectionError_, TimeoutError_) as exc:
            client.close()
            self._wifi_client = None
            self._connected = False
            self._connection_desc = ""
            raise RuntimeError(f"WiFi 连接失败: {exc}") from exc

        self._serial = None
        self._wifi_client = client
        self._transport_mode = "wifi"
        self._connected = True
        self._connection_desc = f"{host}:{port}"
        self.status_signal.emit(f"WiFi 已连接: {host}:{port}")

    def is_connected(self) -> bool:
        """返回当前设备连接状态，供界面层做前置校验。"""

        if not self._connected:
            return False
        if self._transport_mode == "wifi":
            return self._wifi_client is not None and self._wifi_client.connected
        return self._serial is not None and self._serial.is_open

    def disconnect_port(self) -> None:
        self.disconnect_transport()

    def disconnect_transport(self) -> None:
        if self._serial is not None:
            try:
                self._serial.close()
            except serial.SerialException:
                pass
        if self._wifi_client is not None:
            self._wifi_client.close()
        self._serial = None
        self._wifi_client = None
        self._connected = False
        self._connection_desc = ""
        self.status_signal.emit("设备连接已断开")

    def enqueue_gcode(self, gcode_lines: List[str]) -> None:
        if not gcode_lines:
            raise RuntimeError("G-Code 队列为空，无法发送")
        if not self.is_connected():
            raise RuntimeError("设备未连接，请先连接串口或 WiFi 后再发送任务")
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
        self.disconnect_transport()

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
        if not self.is_connected():
            return
        if self._transport_mode == "wifi":
            try:
                if self._wifi_client is not None:
                    self._wifi_client.send_line("M5", timeout=2.0)
            except Exception as exc:  # pragma: no cover - 网络环境相关
                self.log_signal.emit(f"[ERR] WiFi 急停指令发送失败: {exc}")
            return

        try:
            self._write_line("M5")
            # Grbl 风格实时急停指令，不走换行协议。
            if self._serial is not None:
                self._serial.write(b"!")
                self._serial.flush()
                self.log_signal.emit("[TX] !")
        except Exception as exc:  # pragma: no cover - 硬件环境相关
            self.log_signal.emit(f"[ERR] 急停指令发送失败: {exc}")

    def _send_job(self, gcode_lines: List[str]) -> None:
        lines = self._normalize_gcode_lines(gcode_lines)
        total = len(lines)
        if total == 0:
            raise RuntimeError("发送队列为空或仅包含注释，无法执行任务")

        for index, line in enumerate(lines, start=1):
            if self._emergency_stop:
                self._send_emergency_commands()
                raise RuntimeError("任务被急停终止")

            if self._transport_mode == "wifi":
                if self._wifi_client is None:
                    raise RuntimeError("WiFi 未连接，无法发送")
                response = self._wifi_client.send_line(line, timeout=self._wait_ok_timeout)
                if response.startswith("error"):
                    raise RuntimeError(f"下位机返回错误: {response}")
            else:
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

                if not self.is_connected():
                    self.finished_signal.emit(False, "设备未连接，无法执行任务")
                    continue

                self._emergency_stop = False
                self.progress_signal.emit(0)
                if self._transport_mode == "wifi":
                    self.status_signal.emit("开始通过 WiFi 发送 G-Code")
                else:
                    self.status_signal.emit("开始通过串口发送 G-Code")
                self._send_job(job)
                self.progress_signal.emit(100)
                self.finished_signal.emit(True, "任务执行完成")
            except Exception as exc:  # pragma: no cover - 运行时相关
                self.finished_signal.emit(False, str(exc))
                if self._emergency_stop:
                    self._send_emergency_commands()
                    self._emergency_stop = False
