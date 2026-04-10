#!/usr/bin/env python3
"""
WS63 WiFi G-Code 共享传输层。

封装与发射板 WiFi TCP 入口的所有通信语义，供网页控制台、TCP 验证工具
和桥接脚本共同复用。仅使用 Python 标准库 (socket / threading / logging)。

板端协议要点 (来自 wifi_gcode_server.c):
  - 建连后板端主动发 4 行欢迎信息，最后一行 "[MSG:One upstream host at a time]"
  - 每条 G-Code / $ 命令板端回 "ok\\r\\n" 或 "error:...\\r\\n"
  - "?" 状态查询回 "<Idle|MPos:x,y,z|FS:f,s>" 格式，**不回 ok**
  - "$WIFI?" 回 "[WIFI:MODE=...,SLE=...]\\r\\nok\\r\\n"
  - 空行 (len==0) 板端静默忽略，不回任何内容
"""

from __future__ import annotations

import logging
import re
import socket
import threading
import time
from typing import Callable, Dict, List, Optional, Tuple

__all__ = [
    "WifiGcodeClient",
    "ConnectionError_",
    "TimeoutError_",
    "parse_status_line",
    "parse_wifi_status_line",
]

logger = logging.getLogger("wifi_client")

# ---------------------------------------------------------------------------
# 自定义异常
# ---------------------------------------------------------------------------

class ConnectionError_(Exception):
    """连接或发送失败。"""

class TimeoutError_(Exception):
    """等待响应超时。"""

# ---------------------------------------------------------------------------
# 响应解析工具
# ---------------------------------------------------------------------------

STATUS_RE = re.compile(
    r"^<(?P<state>[^|>]+)\|MPos:(?P<x>-?\d+(?:\.\d+)?),"
    r"(?P<y>-?\d+(?:\.\d+)?),(?P<z>-?\d+(?:\.\d+)?)"
    r"\|FS:(?P<f>-?\d+(?:\.\d+)?),(?P<s>-?\d+(?:\.\d+)?)>$"
)

WIFI_STATUS_RE = re.compile(
    r"^\[WIFI:"
    r"MODE=(?P<mode>[^,]+),"
    r"SSID=(?P<ssid>[^,]+),"
    r"IF=(?P<ifname>[^,]+),"
    r"IP=(?P<ip>[^,]+),"
    r"NET=(?P<net>\d+),"
    r"TCP=(?P<tcp>\d+),"
    r"CLI=(?P<cli>\d+),"
    r"APSTA=(?P<apsta>\d+),"
    r"STALINK=(?P<stalink>\d+),"
    r"SLE=(?P<sle>\d+),"
    r"REASON=(?P<reason>-?\d+)"
    r"\]$"
)


def parse_status_line(line: str) -> Optional[Dict[str, object]]:
    """解析 Grbl 状态行 ``<Idle|MPos:...|FS:...>``。"""
    m = STATUS_RE.match(line)
    if not m:
        return None
    return {
        "state": m.group("state"),
        "x": float(m.group("x")),
        "y": float(m.group("y")),
        "z": float(m.group("z")),
        "feed": float(m.group("f")),
        "spindle": float(m.group("s")),
        "raw": line,
    }


def parse_wifi_status_line(line: str) -> Optional[Dict[str, object]]:
    """解析 ``[WIFI:MODE=...,SLE=...]`` 行。"""
    m = WIFI_STATUS_RE.match(line)
    if not m:
        return None
    return {
        "mode": m.group("mode"),
        "ssid": m.group("ssid"),
        "ifname": m.group("ifname"),
        "ip": m.group("ip"),
        "net_ready": m.group("net") == "1",
        "tcp_listening": m.group("tcp") == "1",
        "client_connected": m.group("cli") == "1",
        "softap_sta_count": int(m.group("apsta")),
        "sta_link_up": m.group("stalink") == "1",
        "sle_ready": m.group("sle") == "1",
        "last_disconnect_reason": int(m.group("reason")),
        "raw": line,
    }


# ---------------------------------------------------------------------------
# WifiGcodeClient
# ---------------------------------------------------------------------------

class WifiGcodeClient:
    """发射板 WiFi TCP G-Code 入口的 Python 客户端。

    典型用法::

        client = WifiGcodeClient()
        client.connect("192.168.43.1", 5000)
        intro = client.read_intro()
        wifi = client.query_wifi_status()
        client.send_line("G90")          # -> "ok"
        status = client.query_status()   # -> {"state": "Idle", ...}
        client.send_job(["G90", "M3 S200", "G1 X10 Y10 F6000", "M5"])
        client.close()
    """

    DEFAULT_TIMEOUT = 5.0

    def __init__(self) -> None:
        self._sock: Optional[socket.socket] = None
        self._lock = threading.Lock()
        self._recv_buf = bytearray()
        self._log_lines: List[str] = []
        self._log_lock = threading.Lock()
        self._on_line: Optional[Callable[[str], None]] = None
        self._connected = False

    # -- 连接管理 -----------------------------------------------------------

    def connect(
        self,
        host: str,
        port: int = 5000,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> None:
        """建立到发射板 WiFi TCP 口的连接。"""
        if self._connected:
            raise ConnectionError_("already connected; call close() first")
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect((host, port))
        except (OSError, socket.error) as exc:
            sock.close()
            raise ConnectionError_(f"connect {host}:{port} failed: {exc}") from exc
        self._sock = sock
        self._recv_buf.clear()
        self._connected = True
        logger.info("connected to %s:%d", host, port)

    def close(self) -> None:
        """优雅断开连接。"""
        if self._sock is not None:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()
            self._sock = None
        self._connected = False
        self._recv_buf.clear()
        logger.info("disconnected")

    @property
    def connected(self) -> bool:
        return self._connected

    # -- 行读取 -------------------------------------------------------------

    def _read_line(self, timeout: float = DEFAULT_TIMEOUT) -> Optional[str]:
        """从 socket 中读取一个 ``\\r\\n`` 结尾的行，去除末尾换行。

        返回 None 表示超时未读到完整行。
        socket 关闭或出错时抛 ConnectionError_。
        """
        if self._sock is None:
            raise ConnectionError_("not connected")

        deadline = time.monotonic() + timeout
        while True:
            # 检查缓冲区是否已有完整行
            idx = self._recv_buf.find(b"\n")
            if idx >= 0:
                raw = bytes(self._recv_buf[: idx + 1])
                del self._recv_buf[: idx + 1]
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                self._append_log(line)
                return line

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None

            self._sock.settimeout(min(remaining, 0.5))
            try:
                chunk = self._sock.recv(1024)
            except socket.timeout:
                continue
            except OSError as exc:
                self._connected = False
                raise ConnectionError_(f"recv error: {exc}") from exc
            if not chunk:
                self._connected = False
                raise ConnectionError_("connection closed by remote")
            self._recv_buf.extend(chunk)

    # -- 日志收集 -----------------------------------------------------------

    def set_on_line(self, callback: Optional[Callable[[str], None]]) -> None:
        """注册逐行回调，用于网页控制台日志面板等场景。"""
        self._on_line = callback

    def _append_log(self, line: str) -> None:
        with self._log_lock:
            self._log_lines.append(line)
            if len(self._log_lines) > 5000:
                self._log_lines = self._log_lines[-3000:]
        cb = self._on_line
        if cb is not None:
            try:
                cb(line)
            except Exception:
                pass

    def get_logs(self, since: int = 0) -> Tuple[List[str], int]:
        """返回 ``(lines, next_cursor)``。调用方用 *since* 做增量拉取。"""
        with self._log_lock:
            lines = self._log_lines[since:]
            return list(lines), len(self._log_lines)

    def clear_logs(self) -> None:
        with self._log_lock:
            self._log_lines.clear()

    # -- 欢迎信息 -----------------------------------------------------------

    def read_intro(self, timeout: float = DEFAULT_TIMEOUT) -> List[str]:
        """读取建连后板端主动发送的欢迎信息 (通常 4~5 行)。

        读到包含 ``[MSG:One upstream host at a time]`` 的行即停止。
        """
        lines: List[str] = []
        while True:
            line = self._read_line(timeout=timeout)
            if line is None:
                break
            lines.append(line)
            if "One upstream host at a time" in line:
                break
        return lines

    # -- 核心发送 -----------------------------------------------------------

    def _send_raw(self, data: str) -> None:
        if self._sock is None:
            raise ConnectionError_("not connected")
        try:
            self._sock.sendall((data + "\r\n").encode("utf-8"))
        except OSError as exc:
            self._connected = False
            raise ConnectionError_(f"send error: {exc}") from exc
        self._append_log(f"> {data}")

    def send_line(
        self,
        line: str,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> str:
        """发送单行 G-Code 或 $ 命令，等待 ``ok`` / ``error``。

        返回完整的 ``ok`` 或 ``error:...`` 响应字符串。
        对于 ``$I`` / ``$G`` / ``$CAP?`` 等有多行输出的命令，中间行也会被收集到日志中，
        最终仍返回收到的 ``ok`` 行。
        """
        with self._lock:
            self._send_raw(line)
            return self._wait_ok(timeout)

    def _wait_ok(self, timeout: float) -> str:
        """读取直到收到 ``ok`` 或 ``error:*``。"""
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError_(f"timeout waiting for ok ({timeout:.1f}s)")
            resp = self._read_line(timeout=remaining)
            if resp is None:
                raise TimeoutError_(f"timeout waiting for ok ({timeout:.1f}s)")
            if resp == "ok":
                return "ok"
            if resp.startswith("error"):
                return resp

    # -- 状态查询 -----------------------------------------------------------

    def query_status(self, timeout: float = DEFAULT_TIMEOUT) -> Dict[str, object]:
        """发送 ``?`` 并解析返回的 ``<Idle|MPos:...|FS:...>`` 行。

        注意：板端 ``?`` **不回 ok**。
        """
        with self._lock:
            self._send_raw("?")
            deadline = time.monotonic() + timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError_("timeout waiting for status response")
                line = self._read_line(timeout=remaining)
                if line is None:
                    raise TimeoutError_("timeout waiting for status response")
                parsed = parse_status_line(line)
                if parsed is not None:
                    return parsed

    def query_wifi_status(
        self, timeout: float = DEFAULT_TIMEOUT
    ) -> Dict[str, object]:
        """发送 ``$WIFI?`` 并解析返回。板端回 ``[WIFI:...] + ok``。"""
        with self._lock:
            self._send_raw("$WIFI?")
            result: Optional[Dict[str, object]] = None
            deadline = time.monotonic() + timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError_("timeout waiting for $WIFI? response")
                line = self._read_line(timeout=remaining)
                if line is None:
                    raise TimeoutError_("timeout waiting for $WIFI? response")
                if result is None:
                    parsed = parse_wifi_status_line(line)
                    if parsed is not None:
                        result = parsed
                if line == "ok":
                    if result is not None:
                        return result
                    raise TimeoutError_("got ok but no [WIFI:...] payload")

    # -- 任务发送 -----------------------------------------------------------

    def send_job(
        self,
        lines: List[str],
        timeout_per_line: float = DEFAULT_TIMEOUT,
        on_progress: Optional[Callable[[int, int, str], None]] = None,
    ) -> List[Tuple[int, str, str]]:
        """逐行发送 G-Code 任务，保留"发一行等一行"语义。

        参数:
            lines: G-Code 行列表。
            timeout_per_line: 每行等 ok 的超时秒数。
            on_progress: ``(line_index, total, response)`` 进度回调。

        返回: ``[(index, sent_line, response), ...]``
        """
        results: List[Tuple[int, str, str]] = []
        total = len(lines)
        for i, raw_line in enumerate(lines):
            stripped = raw_line.strip()
            # 跳过空行和纯注释行
            if not stripped or stripped.startswith(";") or stripped.startswith("("):
                continue
            # 去掉行内注释
            if ";" in stripped:
                stripped = stripped[: stripped.index(";")].rstrip()
            if not stripped:
                continue
            resp = self.send_line(stripped, timeout=timeout_per_line)
            results.append((i, stripped, resp))
            if on_progress:
                try:
                    on_progress(i, total, resp)
                except Exception:
                    pass
            if resp.startswith("error"):
                logger.warning("job stopped at line %d: %s -> %s", i, stripped, resp)
                break
        return results

    # -- 辅助 ---------------------------------------------------------------

    def drain(self, quiet_s: float = 0.3) -> List[str]:
        """排空接收缓冲区中的残留数据。"""
        lines: List[str] = []
        deadline = time.monotonic() + quiet_s
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            line = self._read_line(timeout=remaining)
            if line is None:
                break
            lines.append(line)
            deadline = time.monotonic() + quiet_s  # 有数据就续期
        return lines
