#!/usr/bin/env python3
"""
WS63 WiFi 本地网页控制台 —— 后端。

使用 Python 标准库 ``http.server`` 实现，Python 运行零第三方依赖。
后端负责通过 ``WifiGcodeClient`` 透传到板端 TCP G-Code 入口，
前端通过 HTTP JSON API 控制。

启动::

    python3 wifi_console.py                      # 默认 http://localhost:8080
    python3 wifi_console.py --web-port 9000       # 自定义 Web 端口

所有 API 路径:
    POST /api/connect          {"host":"192.168.43.1","port":5000}
    POST /api/disconnect
    GET  /api/status
    POST /api/send-line        {"line":"G90"}
    POST /api/send-job         {"lines":["G90","M3 S200","G1 X10 Y10 F6000","M5"]}
    POST /api/preset/smoke
    POST /api/logs/clear
    GET  /api/logs?since=0
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from typing import Any, Dict, List, Optional

from wifi_client import (
    WifiGcodeClient,
    ConnectionError_,
    TimeoutError_,
)

logger = logging.getLogger("wifi_console")

# ---------------------------------------------------------------------------
# Smoke 预置任务 (来自 uart_auto_test.py run_smoke_case)
# ---------------------------------------------------------------------------
SMOKE_COMMANDS: List[str] = [
    "G90",
    "G92",
    "M3 S200",
    "G1 X10 Y10 F6000",
    "G1 X20 Y10",
    "M5",
]

# ---------------------------------------------------------------------------
# 全局状态
# ---------------------------------------------------------------------------

class AppState:
    """后端全局状态容器。"""

    def __init__(self) -> None:
        self.client = WifiGcodeClient()
        self.lock = threading.Lock()
        self.host: str = ""
        self.port: int = 5000
        self.intro_lines: List[str] = []
        self.last_wifi_status: Optional[Dict[str, object]] = None
        self.last_motion_status: Optional[Dict[str, object]] = None
        self.job_running = False
        self.job_progress: Optional[Dict[str, Any]] = None

app = AppState()

# ---------------------------------------------------------------------------
# 辅助
# ---------------------------------------------------------------------------

def _json_response(handler: ConsoleHandler, code: int, data: Any) -> None:
    body = json.dumps(data, ensure_ascii=False, default=str).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    handler.wfile.write(body)


def _read_json(handler: ConsoleHandler) -> Dict[str, Any]:
    length = int(handler.headers.get("Content-Length", 0))
    if length == 0:
        return {}
    raw = handler.rfile.read(length)
    return json.loads(raw.decode("utf-8"))

# ---------------------------------------------------------------------------
# G-Code 行预过滤 (与 WifiGcodeClient.send_job 过滤逻辑保持一致)
# ---------------------------------------------------------------------------

def _filter_sendable_lines(lines: List[str]) -> List[str]:
    """返回去除空行、注释行和行内注释后的可发送行列表。"""
    result: List[str] = []
    for raw in lines:
        s = raw.strip()
        if not s or s.startswith(";") or s.startswith("("):
            continue
        if ";" in s:
            s = s[: s.index(";")].rstrip()
        if s:
            result.append(s)
    return result


def _reset_session_state(clear_connection: bool) -> None:
    """重置当前会话缓存，避免上一次任务污染下一次会话。"""
    app.intro_lines = []
    app.last_wifi_status = None
    app.last_motion_status = None
    app.job_running = False
    app.job_progress = None
    if clear_connection:
        app.host = ""
        app.port = 5000

# ---------------------------------------------------------------------------
# HTTP 接口
# ---------------------------------------------------------------------------

class ConsoleHandler(BaseHTTPRequestHandler):
    """最小 HTTP JSON API。"""

    server_version = "WS63-WiFi-Console/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:  # noqa: D401
        logger.debug(fmt, *args)

    # -- 静态文件 (前端 HTML) -----------------------------------------------

    def _serve_frontend(self) -> None:
        html_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wifi_console.html")
        try:
            with open(html_path, "rb") as f:
                content = f.read()
        except FileNotFoundError:
            self.send_error(404, "wifi_console.html not found")
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    # -- routing ------------------------------------------------------------

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self) -> None:
        if self.path == "/" or self.path == "/index.html":
            self._serve_frontend()
        elif self.path == "/api/status":
            self._handle_status()
        elif self.path.startswith("/api/logs"):
            self._handle_logs()
        else:
            self.send_error(404)

    def do_POST(self) -> None:
        if self.path == "/api/connect":
            self._handle_connect()
        elif self.path == "/api/disconnect":
            self._handle_disconnect()
        elif self.path == "/api/send-line":
            self._handle_send_line()
        elif self.path == "/api/send-job":
            self._handle_send_job()
        elif self.path == "/api/preset/smoke":
            self._handle_preset_smoke()
        elif self.path == "/api/logs/clear":
            self._handle_logs_clear()
        else:
            self.send_error(404)

    # -- API 实现 -----------------------------------------------------------

    def _handle_connect(self) -> None:
        params = _read_json(self)
        host = params.get("host", "192.168.43.1")
        port = int(params.get("port", 5000))
        timeout = float(params.get("timeout", 5))

        with app.lock:
            if app.client.connected:
                app.client.close()
            app.client.clear_logs()
            _reset_session_state(clear_connection=False)
            try:
                app.client.connect(host, port, timeout=timeout)
                app.host = host
                app.port = port
                app.intro_lines = app.client.read_intro(timeout=timeout)
                # 自动拉一次 $WIFI?
                try:
                    app.last_wifi_status = app.client.query_wifi_status(timeout=3)
                except (TimeoutError_, ConnectionError_):
                    app.last_wifi_status = None
            except ConnectionError_ as exc:
                _json_response(self, 502, {"ok": False, "error": str(exc)})
                return

        _json_response(self, 200, {
            "ok": True,
            "intro": app.intro_lines,
            "wifi_status": app.last_wifi_status,
        })

    def _handle_disconnect(self) -> None:
        with app.lock:
            app.client.close()
            app.client.clear_logs()
            _reset_session_state(clear_connection=False)
        _json_response(self, 200, {"ok": True, "cursor": 0})

    def _handle_status(self) -> None:
        connected = app.client.connected
        result: Dict[str, Any] = {
            "connected": connected,
            "host": app.host,
            "port": app.port,
            "wifi_status": app.last_wifi_status,
            "motion_status": app.last_motion_status,
            "job_running": app.job_running,
            "job_progress": app.job_progress,
        }
        if connected:
            try:
                with app.lock:
                    app.last_motion_status = app.client.query_status(timeout=2)
                result["motion_status"] = app.last_motion_status
                result["motion_status_error"] = None
            except TimeoutError_ as exc:
                result["motion_status_error"] = str(exc)
            except ConnectionError_ as exc:
                result["motion_status_error"] = str(exc)
                result["connected"] = False
                app.last_motion_status = None
                app.last_wifi_status = None
                result["motion_status"] = None
                result["wifi_status"] = None
                _json_response(self, 200, result)
                return
            try:
                with app.lock:
                    app.last_wifi_status = app.client.query_wifi_status(timeout=2)
                result["wifi_status"] = app.last_wifi_status
                result["wifi_status_error"] = None
            except TimeoutError_ as exc:
                result["wifi_status_error"] = str(exc)
            except ConnectionError_ as exc:
                result["wifi_status_error"] = str(exc)
                result["connected"] = False
                app.last_motion_status = None
                app.last_wifi_status = None
                result["motion_status"] = None
                result["wifi_status"] = None
                _json_response(self, 200, result)
                return
        else:
            result["motion_status"] = None
            result["wifi_status"] = None
        _json_response(self, 200, result)

    def _handle_send_line(self) -> None:
        params = _read_json(self)
        line = params.get("line", "").strip()
        if not line:
            _json_response(self, 400, {"ok": False, "error": "empty line"})
            return
        timeout = float(params.get("timeout", 5))
        with app.lock:
            if not app.client.connected:
                _json_response(self, 400, {"ok": False, "error": "not connected"})
                return
            try:
                # ? 查询走专用路径
                if line == "?":
                    status = app.client.query_status(timeout=timeout)
                    app.last_motion_status = status
                    _json_response(self, 200, {"ok": True, "response": status.get("raw", ""), "status": status})
                    return
                # $WIFI? 走专用路径
                if line in ("$WIFI?", "$WIFI"):
                    ws = app.client.query_wifi_status(timeout=timeout)
                    app.last_wifi_status = ws
                    _json_response(self, 200, {"ok": True, "response": ws.get("raw", ""), "wifi_status": ws})
                    return
                resp = app.client.send_line(line, timeout=timeout)
                _json_response(self, 200, {"ok": True, "response": resp})
            except (TimeoutError_, ConnectionError_) as exc:
                _json_response(self, 502, {"ok": False, "error": str(exc)})

    def _handle_send_job(self) -> None:
        params = _read_json(self)
        lines = params.get("lines", [])
        if not lines:
            _json_response(self, 400, {"ok": False, "error": "empty job"})
            return
        if app.job_running:
            _json_response(self, 409, {"ok": False, "error": "job already running"})
            return
        # 预过滤: 只计实际可发送行，使进度 100% 准确
        sendable = _filter_sendable_lines(lines)
        if not sendable:
            _json_response(self, 400, {"ok": False, "error": "no sendable lines"})
            return
        total = len(sendable)
        # 异步执行 — 不持有 app.lock，send_line 内部自带锁
        app.job_running = True
        app.job_progress = {"sent": 0, "total": total, "status": "running"}
        def run_job() -> None:
            try:
                def on_progress(idx: int, _total: int, resp: str) -> None:
                    app.job_progress = {
                        "sent": idx + 1,
                        "total": total,
                        "status": "running",
                        "last_response": resp,
                    }
                results = app.client.send_job(sendable, on_progress=on_progress)
                app.job_progress = {
                    "sent": len(results),
                    "total": total,
                    "status": "done",
                    "results": [(i, l, r) for i, l, r in results[-5:]],
                }
            except Exception as exc:
                app.job_progress = {"status": "error", "error": str(exc)}
            finally:
                app.job_running = False
        t = threading.Thread(target=run_job, daemon=True)
        t.start()
        _json_response(self, 202, {"ok": True, "message": "job started", "total": total})

    def _handle_preset_smoke(self) -> None:
        if app.job_running:
            _json_response(self, 409, {"ok": False, "error": "job already running"})
            return
        if not app.client.connected:
            _json_response(self, 400, {"ok": False, "error": "not connected"})
            return
        total = len(SMOKE_COMMANDS)
        app.job_running = True
        app.job_progress = {"sent": 0, "total": total, "status": "running"}
        def run_smoke() -> None:
            try:
                def on_progress(idx: int, _total: int, resp: str) -> None:
                    app.job_progress = {"sent": idx + 1, "total": total, "status": "running", "last_response": resp}
                results = app.client.send_job(SMOKE_COMMANDS, on_progress=on_progress)
                app.job_progress = {
                    "sent": len(results),
                    "total": total,
                    "status": "done",
                    "results": [(i, l, r) for i, l, r in results],
                }
            except Exception as exc:
                app.job_progress = {"status": "error", "error": str(exc)}
            finally:
                app.job_running = False
        t = threading.Thread(target=run_smoke, daemon=True)
        t.start()
        _json_response(self, 202, {"ok": True, "message": "smoke started"})

    def _handle_logs(self) -> None:
        # 从 query string 解析 since
        since = 0
        if "?" in self.path:
            qs = self.path.split("?", 1)[1]
            for part in qs.split("&"):
                if part.startswith("since="):
                    try:
                        since = int(part.split("=", 1)[1])
                    except ValueError:
                        pass
        lines, cursor = app.client.get_logs(since=since)
        _json_response(self, 200, {"lines": lines, "cursor": cursor})

    def _handle_logs_clear(self) -> None:
        with app.lock:
            app.client.clear_logs()
        _json_response(self, 200, {"ok": True, "cursor": 0})


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="WS63 WiFi 网页控制台")
    p.add_argument("--web-port", type=int, default=8080, help="本地 HTTP 端口 (默认 8080)")
    p.add_argument("--bind", default="0.0.0.0", help="绑定地址 (默认 0.0.0.0)")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


def main() -> None:
    args = build_arg_parser().parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(name)s] %(levelname)s %(message)s",
    )
    server = HTTPServer((args.bind, args.web_port), ConsoleHandler)
    logger.info("WS63 WiFi Console listening on http://%s:%d", args.bind, args.web_port)
    logger.info("Open http://localhost:%d in your browser", args.web_port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("shutting down")
    finally:
        app.client.close()
        server.server_close()


if __name__ == "__main__":
    main()
