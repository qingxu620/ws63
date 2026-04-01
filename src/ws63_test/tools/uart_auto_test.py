#!/usr/bin/env python3
"""
WS63 UART auto test tool for the ws63_test transmitter board.

This script talks to the transmitter business UART (UART1) and runs
deterministic test scenarios. Optional debug serial ports can be used to
watch transmitter/receiver UART0 logs and wait until the SLE link is ready.

Typical usage:

  python3 tools/uart_auto_test.py --list-ports
  python3 tools/uart_auto_test.py --pick-ports --suite smoke
  python3 tools/uart_auto_test.py /dev/ttyUSB1 --suite smoke
  python3 tools/uart_auto_test.py /dev/ttyUSB1 --tx-debug-port /dev/ttyUSB0 --suite square
  python3 tools/uart_auto_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite all --report-json result.json

Requirements:
  pip install pyserial
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import re
import sys
import threading
import time
from collections import deque
from dataclasses import asdict, dataclass
from datetime import datetime
from typing import Callable, Deque, List, Optional, Sequence

try:
    import serial
    from serial import Serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - import guard for user environment
    raise SystemExit("pyserial is required: pip install pyserial") from exc


STATUS_RE = re.compile(
    r"^<(?P<state>[^|>]+)\|MPos:(?P<x>-?\d+(?:\.\d+)?),(?P<y>-?\d+(?:\.\d+)?),(?P<z>-?\d+(?:\.\d+)?)\|FS:(?P<f>-?\d+(?:\.\d+)?),(?P<s>-?\d+(?:\.\d+)?)>$"
)
READY_RE = re.compile(r"status link ready|hb stat conn=1 handles=1 status_rx=1|heartbeat rx=", re.IGNORECASE)
STATUS_QUERY_RESEND_INTERVAL_S = 0.5

DEFAULT_BUSINESS_PORT = "COM8"
DEFAULT_TX_DEBUG_PORT: Optional[str] = "COM11"
DEFAULT_RX_DEBUG_PORT: Optional[str] = "COM13"

# 配置文件名 — 放在脚本同目录，用于持久化端口设置
CONFIG_FILE_NAME = "test_config.json"

logger = logging.getLogger("ws63_test")


def _script_dir() -> str:
    """Return the directory that contains this script."""
    return os.path.dirname(os.path.abspath(__file__))


def load_config_file(config_path: Optional[str] = None) -> dict:
    """Load test_config.json from *config_path* or the script directory.

    Returns an empty dict if the file does not exist.
    Supported keys:
        business_port, tx_debug_port, rx_debug_port, baud, debug_baud,
        no_tx_debug, no_rx_debug
    """
    if config_path is None:
        config_path = os.path.join(_script_dir(), CONFIG_FILE_NAME)
    if not os.path.isfile(config_path):
        return {}
    try:
        with open(config_path, "r", encoding="utf-8") as fp:
            cfg = json.load(fp)
        logger.info("Loaded config from %s", config_path)
        return cfg if isinstance(cfg, dict) else {}
    except (json.JSONDecodeError, OSError) as exc:
        print(f"[WARN] Failed to load {config_path}: {exc}", file=sys.stderr)
        return {}


def apply_config_defaults(args: argparse.Namespace, cfg: dict) -> None:
    """Apply config-file values as defaults — CLI args take priority."""
    mapping = {
        "business_port": "port",
        "tx_debug_port": "tx_debug_port",
        "rx_debug_port": "rx_debug_port",
        "baud": "baud",
        "debug_baud": "debug_baud",
        "no_tx_debug": "no_tx_debug",
        "no_rx_debug": "no_rx_debug",
    }
    for cfg_key, attr in mapping.items():
        if cfg_key in cfg:
            # Only override if the user did NOT explicitly set the CLI arg
            current = getattr(args, attr, None)
            default_val = {
                "port": DEFAULT_BUSINESS_PORT,
                "tx_debug_port": DEFAULT_TX_DEBUG_PORT,
                "rx_debug_port": DEFAULT_RX_DEBUG_PORT,
                "baud": 115200,
                "debug_baud": 115200,
                "no_tx_debug": False,
                "no_rx_debug": False,
            }.get(attr)
            if current == default_val:
                setattr(args, attr, cfg[cfg_key])


def setup_file_logging(log_dir: str, prefix: str = "ws63_test") -> str:
    """Configure logging to write to a timestamped file in *log_dir*.

    Returns the log file path.
    """
    os.makedirs(log_dir, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(log_dir, f"{prefix}_{timestamp}.log")
    handler = logging.FileHandler(log_path, encoding="utf-8")
    handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s"))
    logging.getLogger().addHandler(handler)
    logging.getLogger().setLevel(logging.DEBUG)
    logger.info("Log file: %s", log_path)
    return log_path


def windows_serial_hint(port: str, exc: Exception) -> str:
    """Return a human-readable troubleshooting hint for Windows COM port errors."""
    lines = [
        f"无法打开串口 {port}: {exc}",
        "",
        "排查建议 (Windows):",
        f"  1. 打开 设备管理器 → 端口(COM和LPT)，确认 {port} 存在",
        "  2. 确认没有其他程序 (串口助手/LaserGRBL) 占用该端口",
        "  3. 如果端口号变了，请更新 test_config.json 或命令行参数",
        "  4. 尝试拔插 USB 线后重新运行",
        "  5. 确认 USB 转串口驱动已正确安装 (CH340/CP210x/FTDI)",
    ]
    return "\n".join(lines)


class TestFailure(RuntimeError):
    """Raised when a test case fails."""


@dataclass
class MotionStatus:
    state: str
    x: float
    y: float
    z: float
    feed: float
    spindle: float
    raw: str


@dataclass
class TestResult:
    name: str
    passed: bool
    detail: str
    elapsed_s: float


def resolve_suites(selected_suite: str) -> List[str]:
    return [selected_suite] if selected_suite != "all" else ["smoke", "square", "repeat"]


def write_report(
    path: str,
    args: argparse.Namespace,
    results: Sequence[TestResult],
    exit_code: int,
    failure_detail: Optional[str],
    recent_debug_lines: Optional[dict[str, List[str]]] = None,
) -> None:
    payload = {
        "timestamp": datetime.now().astimezone().isoformat(timespec="seconds"),
        "exit_code": exit_code,
        "passed": exit_code == 0,
        "failure_detail": failure_detail,
        "port": args.port,
        "baud": args.baud,
        "suite": args.suite,
        "resolved_suites": resolve_suites(args.suite),
        "rounds": args.rounds,
        "tx_debug_port": None if args.no_tx_debug else args.tx_debug_port,
        "rx_debug_port": None if args.no_rx_debug else args.rx_debug_port,
        "results": [asdict(result) for result in results],
        "recent_debug_lines": recent_debug_lines or {},
    }
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(payload, fp, indent=2, ensure_ascii=False)
        fp.write("\n")


def get_available_ports() -> List[object]:
    return sorted(list(list_ports.comports()), key=lambda item: item.device.upper())


def describe_port(port_info: object) -> str:
    description = getattr(port_info, "description", "") or "Unknown device"
    hwid = getattr(port_info, "hwid", "") or "N/A"
    return f"{port_info.device:<12} {description} [{hwid}]"


def print_available_ports() -> List[object]:
    ports = get_available_ports()
    if not ports:
        print("No serial ports detected.")
        return ports

    print("Available serial ports:")
    for index, port_info in enumerate(ports, start=1):
        print(f"  {index:>2}. {describe_port(port_info)}")
    return ports


def select_port_interactively(
    label: str,
    ports: Sequence[object],
    allow_empty: bool = False,
    excluded_ports: Optional[Sequence[str]] = None,
) -> Optional[str]:
    excluded = {item.upper() for item in (excluded_ports or []) if item}
    selectable = [port_info for port_info in ports if port_info.device.upper() not in excluded]

    if not selectable:
        if allow_empty:
            return None
        raise TestFailure(f"No selectable serial port remains for {label}")

    print(f"{label}:")
    for index, port_info in enumerate(selectable, start=1):
        print(f"  {index:>2}. {describe_port(port_info)}")

    if allow_empty:
        print("  Enter to skip")

    while True:
        choice = input(f"Select {label}: ").strip()
        if not choice and allow_empty:
            return None
        if choice.isdigit():
            index = int(choice)
            if 1 <= index <= len(selectable):
                return selectable[index - 1].device

        normalized = choice.upper()
        for port_info in selectable:
            if port_info.device.upper() == normalized:
                return port_info.device

        prompt_hint = "index or port name" + ("; Enter to skip" if allow_empty else "")
        print(f"Invalid selection, please enter {prompt_hint}.")


def resolve_ports(args: argparse.Namespace) -> argparse.Namespace:
    if args.list_ports:
        print_available_ports()
        raise SystemExit(0)

    needs_selection = args.pick_ports or args.port is None
    if not needs_selection:
        return args

    if not sys.stdin.isatty():
        raise SystemExit("Business UART port is required. Use --list-ports to inspect available ports first.")

    ports = print_available_ports()
    if not ports:
        raise SystemExit("No serial ports detected. Please check the USB-UART adapters and try again.")

    if args.port is None:
        print("Business UART port was not provided, please select it below.")
        args.port = select_port_interactively("Business UART (transmitter UART1)", ports, allow_empty=False)

    if args.pick_ports:
        excluded = [args.port]
        if not args.no_tx_debug and not args.tx_debug_port:
            args.tx_debug_port = select_port_interactively(
                "Transmitter debug UART0", ports, allow_empty=True, excluded_ports=excluded
            )
            excluded.append(args.tx_debug_port or "")
        if not args.no_rx_debug and not args.rx_debug_port:
            args.rx_debug_port = select_port_interactively(
                "Receiver debug UART0", ports, allow_empty=True, excluded_ports=excluded
            )

    return args


class SerialLineChannel:
    """Small helper that reads newline-terminated lines from a serial port."""

    def __init__(self, ser: Serial, label: str, verbose: bool = False) -> None:
        self.ser = ser
        self.label = label
        self.verbose = verbose
        self._buffer = bytearray()
        self._lines: Deque[str] = deque()

    def _pump(self) -> None:
        waiting = self.ser.in_waiting
        if waiting <= 0:
            chunk = self.ser.read(1)
        else:
            chunk = self.ser.read(waiting)
        if not chunk:
            return
        self._buffer.extend(chunk)
        while b"\n" in self._buffer:
            raw, _, rest = self._buffer.partition(b"\n")
            self._buffer = bytearray(rest)
            line = raw.rstrip(b"\r").decode("utf-8", errors="replace").strip()
            if not line:
                continue
            self._lines.append(line)
            if self.verbose:
                print(f"[{self.label}] {line}")

    def read_line(self, timeout: float) -> Optional[str]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._lines:
                return self._lines.popleft()
            self._pump()
            if self._lines:
                return self._lines.popleft()
            time.sleep(0.01)
        return None

    def read_until(self, predicate: Callable[[str], bool], timeout: float) -> Optional[str]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.read_line(max(0.01, deadline - time.monotonic()))
            if line is None:
                continue
            if predicate(line):
                return line
        return None

    def drain(self, quiet_s: float = 0.2) -> List[str]:
        drained: List[str] = []
        deadline = time.monotonic() + quiet_s
        while time.monotonic() < deadline:
            self._pump()
            while self._lines:
                drained.append(self._lines.popleft())
                deadline = time.monotonic() + quiet_s
            time.sleep(0.01)
        return drained

    def reset(self) -> None:
        self.ser.reset_input_buffer()
        self._buffer.clear()
        self._lines.clear()


class DebugMonitor:
    """Background reader for optional UART0 debug output."""

    def __init__(self, port: str, baud: int, label: str, verbose: bool = False) -> None:
        self.port = port
        self.label = label
        self.ser = serial.Serial(port, baudrate=baud, timeout=0.1)
        self.ser.reset_input_buffer()
        self.channel = SerialLineChannel(self.ser, label, verbose=verbose)
        self._recent: Deque[str] = deque(maxlen=200)
        self._ready_seen = False
        self._stop = threading.Event()
        thread_name = f"ws63-{label}-monitor".replace(" ", "-")
        self._thread = threading.Thread(target=self._run, name=thread_name, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            line = self.channel.read_line(0.1)
            if line is None:
                continue
            self._recent.append(line)
            if READY_RE.search(line):
                self._ready_seen = True

    def wait_ready(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._ready_seen:
                return True
            time.sleep(0.05)
        return False

    def recent_lines(self) -> List[str]:
        return list(self._recent)

    def is_ready(self) -> bool:
        return self._ready_seen

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1.0)
        self.ser.close()


def parse_status(line: str) -> Optional[MotionStatus]:
    match = STATUS_RE.match(line)
    if not match:
        return None
    return MotionStatus(
        state=match.group("state"),
        x=float(match.group("x")),
        y=float(match.group("y")),
        z=float(match.group("z")),
        feed=float(match.group("f")),
        spindle=float(match.group("s")),
        raw=line,
    )


def status_matches(status: MotionStatus, x: float, y: float, tolerance: float = 0.05) -> bool:
    return abs(status.x - x) <= tolerance and abs(status.y - y) <= tolerance


class Ws63AutoTester:
    def __init__(
        self,
        port: str,
        baud: int,
        verbose: bool = False,
        debug_monitors: Optional[Sequence[DebugMonitor]] = None,
        default_timeout: float = 3.0,
    ) -> None:
        self.ser = serial.Serial(port, baudrate=baud, timeout=0.1)
        self.channel = SerialLineChannel(self.ser, "uart1", verbose=verbose)
        self.default_timeout = default_timeout
        self.debug_monitors = list(debug_monitors or [])
        time.sleep(0.3)
        self.channel.reset()

    def close(self) -> None:
        self.ser.close()

    def send_line(self, line: str) -> None:
        self.ser.write((line + "\r\n").encode("ascii"))

    def expect_ok(self, line: str, timeout: Optional[float] = None) -> None:
        self.send_line(line)
        deadline = time.monotonic() + (timeout or self.default_timeout)
        while time.monotonic() < deadline:
            resp = self.channel.read_line(max(0.05, deadline - time.monotonic()))
            if resp is None:
                continue
            if resp == "ok":
                return
            if resp.startswith("error"):
                raise TestFailure(f"{line} -> {resp}")
        raise TestFailure(f"{line} -> timeout waiting for ok")

    def expect_motion_ok_or_motion(
        self,
        line: str,
        start_status: MotionStatus,
        expect_x: float,
        expect_y: float,
        ok_timeout: float = 8.0,
        motion_timeout: float = 5.0,
    ) -> Optional[MotionStatus]:
        try:
            self.expect_ok(line, timeout=ok_timeout)
            return None
        except TestFailure as exc:
            if "timeout waiting for ok" not in str(exc):
                raise
            status = self.wait_for_motion_to_start(start_status, expect_x, expect_y, timeout=motion_timeout)
            print(f"[WARN] {line} missing ok but motion observed, continue with {status.raw}")
            return status

    def query_info(self, timeout: float = 3.0) -> List[str]:
        self.send_line("$I")
        ver = None
        opt = None
        got_ok = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            resp = self.channel.read_line(max(0.05, deadline - time.monotonic()))
            if resp is None:
                continue
            if resp.startswith("[VER:"):
                ver = resp
            elif resp.startswith("[OPT:"):
                opt = resp
            elif resp == "ok":
                got_ok = True
            elif resp.startswith("error"):
                raise TestFailure(f"$I -> {resp}")
            if ver and opt and got_ok:
                return [ver, opt]
        raise TestFailure("$I -> timeout waiting for version response")

    def query_status(self, timeout: float = 3.0) -> MotionStatus:
        deadline = time.monotonic() + timeout
        next_send_at = 0.0
        recent_lines: Deque[str] = deque(maxlen=8)
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_send_at:
                self.send_line("?")
                next_send_at = now + STATUS_QUERY_RESEND_INTERVAL_S

            wait_timeout = min(STATUS_QUERY_RESEND_INTERVAL_S, max(0.05, deadline - time.monotonic()))
            resp = self.channel.read_line(wait_timeout)
            if resp is None:
                continue
            status = parse_status(resp)
            if status is not None:
                return status
            recent_lines.append(resp)
            if resp.startswith("error"):
                raise TestFailure(f"? -> {resp}")
        if recent_lines:
            raise TestFailure("? -> timeout waiting for status, recent=" + " | ".join(recent_lines))
        raise TestFailure("? -> timeout waiting for status")

    def wait_until_idle(
        self,
        expect_x: Optional[float] = None,
        expect_y: Optional[float] = None,
        timeout: float = 20.0,
        poll_interval: float = 0.3,
        tolerance: float = 0.05,
        stable_samples: int = 5,
    ) -> MotionStatus:
        deadline = time.monotonic() + timeout
        last_status: Optional[MotionStatus] = None
        stable_hits = 0
        while time.monotonic() < deadline:
            status = self.query_status(timeout=3.0)
            if last_status is not None and last_status.state == status.state and status_matches(
                status, last_status.x, last_status.y, tolerance=tolerance
            ):
                stable_hits += 1
            else:
                stable_hits = 1
            last_status = status
            if status.state == "Idle":
                if expect_x is not None and abs(status.x - expect_x) > tolerance:
                    raise TestFailure(
                        f"Idle reached but X mismatch: got {status.x:.3f}, expect {expect_x:.3f}, raw={status.raw}"
                    )
                if expect_y is not None and abs(status.y - expect_y) > tolerance:
                    raise TestFailure(
                        f"Idle reached but Y mismatch: got {status.y:.3f}, expect {expect_y:.3f}, raw={status.raw}"
                    )
                return status
            if (
                expect_x is not None
                and expect_y is not None
                and stable_hits >= stable_samples
                and status_matches(status, expect_x, expect_y, tolerance=tolerance)
            ):
                print(f"[WARN] target reached but state stayed {status.state}, continue with {status.raw}")
                return status
            time.sleep(poll_interval)
        raise TestFailure(f"timeout waiting for Idle, last_status={last_status.raw if last_status else 'none'}")

    def wait_until_settled(
        self,
        timeout: float = 5.0,
        poll_interval: float = 0.3,
        tolerance: float = 0.05,
        stable_samples: int = 5,
    ) -> MotionStatus:
        deadline = time.monotonic() + timeout
        last_status: Optional[MotionStatus] = None
        stable_hits = 0
        while time.monotonic() < deadline:
            status = self.query_status(timeout=3.0)
            if status.state == "Idle":
                return status

            if last_status is not None and last_status.state == status.state and status_matches(
                status, last_status.x, last_status.y, tolerance=tolerance
            ):
                stable_hits += 1
            else:
                stable_hits = 1
            last_status = status

            if stable_hits >= stable_samples:
                print(f"[WARN] status stayed {status.state} with stable position, use {status.raw} as settled baseline")
                return status
            time.sleep(poll_interval)

        raise TestFailure(f"timeout waiting for settled status, last_status={last_status.raw if last_status else 'none'}")

    def wait_until_position(
        self,
        expect_x: float,
        expect_y: float,
        timeout: float = 20.0,
        poll_interval: float = 0.3,
        tolerance: float = 0.05,
        stall_samples: int = 8,
    ) -> MotionStatus:
        deadline = time.monotonic() + timeout
        last_status: Optional[MotionStatus] = None
        stable_hits = 0
        while time.monotonic() < deadline:
            status = self.query_status(timeout=3.0)
            if status_matches(status, expect_x, expect_y, tolerance=tolerance):
                return status

            if last_status is not None and last_status.state == status.state and status_matches(
                status, last_status.x, last_status.y, tolerance=tolerance
            ):
                stable_hits += 1
            else:
                stable_hits = 1
            last_status = status

            if stable_hits >= stall_samples:
                raise TestFailure(
                    f"position stalled before target ({expect_x:.3f},{expect_y:.3f}), last_status={status.raw}"
                )
            time.sleep(poll_interval)
        raise TestFailure(
            f"timeout waiting for target position ({expect_x:.3f},{expect_y:.3f}), "
            f"last_status={last_status.raw if last_status else 'none'}"
        )

    def wait_for_motion_to_start(
        self,
        start_status: MotionStatus,
        expect_x: float,
        expect_y: float,
        timeout: float = 5.0,
        poll_interval: float = 0.1,
        tolerance: float = 0.05,
    ) -> MotionStatus:
        deadline = time.monotonic() + timeout
        last_status = start_status
        while time.monotonic() < deadline:
            status = self.query_status(timeout=3.0)
            last_status = status
            if status.state == "Run" and start_status.state != "Run":
                return status
            if not status_matches(status, start_status.x, start_status.y, tolerance=tolerance):
                return status
            if status.state == "Idle" and status_matches(status, expect_x, expect_y, tolerance=tolerance):
                return status
            time.sleep(poll_interval)
        raise TestFailure(
            f"timeout waiting for motion start, start={start_status.raw}, last_status={last_status.raw}"
        )

    def move_to_known_position(
        self,
        target_x: float = 0.0,
        target_y: float = 0.0,
        timeout: float = 20.0,
    ) -> MotionStatus:
        current = self.query_status(timeout=3.0)
        if current.state != "Idle":
            current = self.wait_until_settled(timeout=min(timeout, 5.0))
            if current.state != "Idle":
                print(f"[WARN] force clear non-idle baseline with M5: {current.raw}")
                self.expect_ok("M5")
                current = self.wait_until_settled(timeout=min(timeout, 5.0))
        if status_matches(current, target_x, target_y):
            return current

        self.expect_ok("G90")
        start_status = self.query_status(timeout=3.0)
        fallback_status = self.expect_motion_ok_or_motion(
            f"G1 X{target_x:.3f} Y{target_y:.3f} F6000",
            start_status,
            target_x,
            target_y,
            ok_timeout=8.0,
            motion_timeout=5.0,
        )
        if fallback_status is None:
            self.wait_for_motion_to_start(start_status, target_x, target_y, timeout=5.0)
        return self.wait_until_idle(expect_x=target_x, expect_y=target_y, timeout=timeout)

    def wait_ready(self, ready_timeout: float, ready_grace_s: float) -> None:
        if self.debug_monitors:
            deadline = time.monotonic() + ready_timeout
            while time.monotonic() < deadline:
                if any(monitor.is_ready() for monitor in self.debug_monitors):
                    return
                time.sleep(0.05)
            try:
                self.query_status(timeout=3.0)
                return
            except TestFailure:
                pass
            recent_chunks = []
            for monitor in self.debug_monitors:
                recent = "\n".join(monitor.recent_lines()[-10:]) or "<no recent lines>"
                recent_chunks.append(f"[{monitor.label} @ {monitor.port}]\n{recent}")
            raise TestFailure("timeout waiting for debug ready marker\n" + "\n".join(recent_chunks))
            return
        time.sleep(ready_grace_s)
        self.query_status(timeout=3.0)


def run_smoke_case(tester: Ws63AutoTester, idle_timeout: float) -> TestResult:
    start = time.monotonic()
    tester.channel.drain(quiet_s=0.2)
    tester.query_info()
    tester.move_to_known_position(0.0, 0.0, timeout=idle_timeout)
    start_status = tester.query_status(timeout=3.0)
    tester.expect_ok("G90")
    tester.expect_ok("M3 S200")
    tester.expect_ok("G1 X10 Y10 F6000")
    tester.expect_ok("G1 X20 Y10")
    tester.expect_ok("M5")
    tester.wait_for_motion_to_start(start_status, 20.0, 10.0, timeout=5.0)
    status = tester.wait_until_idle(expect_x=20.0, expect_y=10.0, timeout=idle_timeout)
    return TestResult("smoke", True, status.raw, time.monotonic() - start)


def run_square_case(tester: Ws63AutoTester, idle_timeout: float) -> TestResult:
    start = time.monotonic()
    tester.channel.drain(quiet_s=0.2)
    tester.move_to_known_position(0.0, 0.0, timeout=idle_timeout)
    commands = [
        "G90",
        "G1 X0 Y0 F6000",
        "G1 X5 Y0",
        "G1 X10 Y0",
        "G1 X15 Y0",
        "G1 X20 Y0",
        "G1 X20 Y5",
        "G1 X20 Y10",
        "G1 X15 Y10",
        "G1 X10 Y10",
        "G1 X5 Y10",
        "G1 X0 Y10",
        "G1 X0 Y5",
        "G1 X0 Y0",
    ]
    start_status = tester.query_status(timeout=3.0)
    for command in commands:
        tester.expect_ok(command, timeout=5.0)
    tester.wait_for_motion_to_start(start_status, 0.0, 0.0, timeout=5.0)
    status = tester.wait_until_idle(expect_x=0.0, expect_y=0.0, timeout=idle_timeout)
    return TestResult("square", True, status.raw, time.monotonic() - start)


def run_repeat_case(tester: Ws63AutoTester, rounds: int, idle_timeout: float) -> TestResult:
    start = time.monotonic()
    points = [
        (10.0, 10.0),
        (20.0, 10.0),
        (20.0, 20.0),
        (0.0, 20.0),
        (0.0, 0.0),
    ]
    tester.channel.drain(quiet_s=0.2)
    tester.move_to_known_position(0.0, 0.0, timeout=idle_timeout)
    tester.expect_ok("G90")
    tester.expect_ok("M3 S150")
    for index in range(rounds):
        x, y = points[index % len(points)]
        start_status = tester.query_status(timeout=3.0)
        fallback_status = tester.expect_motion_ok_or_motion(
            f"G1 X{x:.3f} Y{y:.3f} F6000",
            start_status,
            x,
            y,
            ok_timeout=8.0,
            motion_timeout=5.0,
        )
        if fallback_status is None:
            tester.wait_for_motion_to_start(start_status, x, y, timeout=5.0)
        tester.wait_until_position(expect_x=x, expect_y=y, timeout=idle_timeout)
    tester.expect_ok("M5")
    status = tester.wait_until_idle(
        expect_x=points[(rounds - 1) % len(points)][0],
        expect_y=points[(rounds - 1) % len(points)][1],
        timeout=idle_timeout,
    )
    return TestResult("repeat", True, f"rounds={rounds}, last={status.raw}", time.monotonic() - start)


def run_named_suite(tester: Ws63AutoTester, suite: str, rounds: int, idle_timeout: float) -> TestResult:
    if suite == "smoke":
        return run_smoke_case(tester, idle_timeout)
    if suite == "square":
        return run_square_case(tester, idle_timeout)
    if suite == "repeat":
        return run_repeat_case(tester, rounds, idle_timeout)
    raise ValueError(f"unsupported suite: {suite}")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WS63 transmitter UART auto test")
    parser.add_argument(
        "port",
        nargs="?",
        default=DEFAULT_BUSINESS_PORT,
        help=f"Business UART port connected to transmitter UART1 (default: {DEFAULT_BUSINESS_PORT})",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Business UART baud rate")
    parser.add_argument(
        "--tx-debug-port",
        "--debug-port",
        dest="tx_debug_port",
        default=DEFAULT_TX_DEBUG_PORT,
        help=f"Transmitter UART0 debug port (default: {DEFAULT_TX_DEBUG_PORT})",
    )
    parser.add_argument(
        "--rx-debug-port",
        default=DEFAULT_RX_DEBUG_PORT,
        help=f"Receiver UART0 debug port (default: {DEFAULT_RX_DEBUG_PORT})",
    )
    parser.add_argument("--no-tx-debug", action="store_true", help="Disable transmitter debug port monitoring")
    parser.add_argument("--no-rx-debug", action="store_true", help="Disable receiver debug port monitoring")
    parser.add_argument("--debug-baud", type=int, default=115200, help="Debug UART baud rate")
    parser.add_argument(
        "--suite",
        choices=("smoke", "square", "repeat", "all"),
        default="all",
        help="Test suite to run",
    )
    parser.add_argument("--rounds", type=int, default=5, help="Repeat count for repeat suite")
    parser.add_argument("--ready-timeout", type=float, default=20.0, help="Timeout waiting for debug ready marker")
    parser.add_argument(
        "--ready-grace",
        type=float,
        default=8.0,
        help="Fallback wait time when no debug port is provided",
    )
    parser.add_argument("--idle-timeout", type=float, default=20.0, help="Timeout waiting for motion to return Idle")
    parser.add_argument("--report-json", help="Write a machine-readable test report to JSON")
    parser.add_argument("--list-ports", action="store_true", help="List available serial ports and exit")
    parser.add_argument(
        "--pick-ports",
        action="store_true",
        help="Interactively select business/debug serial ports before running",
    )
    parser.add_argument("--verbose", action="store_true", help="Print every received line")
    parser.add_argument(
        "--log-dir",
        default=None,
        help="Directory to write timestamped log files (default: no file logging)",
    )
    parser.add_argument(
        "--config",
        default=None,
        help="Path to test_config.json (default: auto-detect in script directory)",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)

    # 加载配置文件 (test_config.json) 并用作默认值
    cfg = load_config_file(getattr(args, "config", None))
    apply_config_defaults(args, cfg)

    # 文件日志
    log_path: Optional[str] = None
    if args.log_dir:
        log_path = setup_file_logging(args.log_dir)
        print(f"Logging to: {log_path}")

    args = resolve_ports(args)

    debug_monitors: List[DebugMonitor] = []
    tester: Optional[Ws63AutoTester] = None
    results: List[TestResult] = []
    failure_detail: Optional[str] = None
    exit_code = 0

    try:
        if not args.no_tx_debug and args.tx_debug_port:
            print(f"Opening transmitter debug UART: {args.tx_debug_port} @ {args.debug_baud}")
            debug_monitors.append(DebugMonitor(args.tx_debug_port, args.debug_baud, "tx-debug", verbose=args.verbose))

        if not args.no_rx_debug and args.rx_debug_port:
            print(f"Opening receiver debug UART: {args.rx_debug_port} @ {args.debug_baud}")
            debug_monitors.append(DebugMonitor(args.rx_debug_port, args.debug_baud, "rx-debug", verbose=args.verbose))

        print(f"Opening business UART: {args.port} @ {args.baud}")
        tester = Ws63AutoTester(args.port, args.baud, verbose=args.verbose, debug_monitors=debug_monitors)

        print("Waiting for link ready...")
        tester.wait_ready(args.ready_timeout, args.ready_grace)
        print("Link ready, start tests.")

        suites = resolve_suites(args.suite)
        for suite in suites:
            result = run_named_suite(tester, suite, args.rounds, args.idle_timeout)
            results.append(result)
            print(f"[PASS] {result.name}: {result.detail} ({result.elapsed_s:.2f}s)")

    except (TestFailure, serial.SerialException, OSError) as exc:
        failure_detail = str(exc)
        exit_code = 1
        # Windows 串口打开失败时给出友好提示
        if isinstance(exc, (serial.SerialException, OSError)) and sys.platform == "win32":
            hint = windows_serial_hint(args.port, exc)
            print(hint, file=sys.stderr)
        else:
            print(f"[FAIL] {failure_detail}", file=sys.stderr)
        logger.error("Test failed: %s", failure_detail)
        for monitor in debug_monitors:
            print(f"Recent {monitor.label} lines ({monitor.port}):", file=sys.stderr)
            for line in monitor.recent_lines()[-20:]:
                print(line, file=sys.stderr)
    finally:
        recent_debug_lines = {
            f"{monitor.label}@{monitor.port}": monitor.recent_lines()[-20:] for monitor in debug_monitors
        }
        if tester is not None:
            tester.close()
        for monitor in debug_monitors:
            monitor.close()
        if args.report_json:
            write_report(args.report_json, args, results, exit_code, failure_detail, recent_debug_lines)

    if exit_code != 0:
        return exit_code

    print("Summary:")
    for result in results:
        print(f"  PASS {result.name:<8} {result.elapsed_s:>6.2f}s  {result.detail}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
