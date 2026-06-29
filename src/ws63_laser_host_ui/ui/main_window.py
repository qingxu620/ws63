"""
Main window — integrates all pages, manages transport, workers, and state.
"""
from __future__ import annotations

import os
import threading
from datetime import datetime
from pathlib import Path
from typing import Optional, TextIO

from PySide6.QtCore import QTimer, Signal
from PySide6.QtGui import QImage
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QMainWindow, QMessageBox, QStackedWidget, QVBoxLayout,
    QWidget,
)

from app.config_store import ConfigStore, HostConfig
from app.event_bus import EventBus
from app.gcode_validator import prepare_gcode_for_upload, format_diagnostic
from app.state_models import AppState, LinkState
from transports.sle_tx_transport import (
    SleJobSerialClient, SerialLogMonitor, available_ports, crc16_ccitt,
    parse_rx_status, prepare_gcode_for_rx, RX_EXEC_DONE_RE,
    JOB_MAX_SIZE, JOB_PREROLL_BYTES,
)
from workers.doubao_image_worker import DoubaoImageWorker
from workers.serial_worker import WorkerManager
from ui.widgets.status_badge import StatusBadge
from ui.widgets.sidebar_nav import SidebarNav
from ui.pages.connection_page import ConnectionPage
from ui.pages.gcode_page import GcodePage
from ui.pages.job_page import JobPage
from ui.pages.logs_page import LogsPage


class MainWindow(QMainWindow):
    focus_state_changed = Signal(bool)
    status_line_received = Signal(str)

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("WS63 激光主控终端")
        self.setMinimumSize(1100, 700)

        # Core
        self.config_store = ConfigStore()
        self.config = self.config_store.load()
        self.event_bus = EventBus()
        self.state = AppState()

        # Transport
        self.client = SleJobSerialClient(self._enqueue_log)
        self.tx_monitor = SerialLogMonitor("tx_log", self._enqueue_log)
        self.rx_monitor = SerialLogMonitor("rx_log", self._enqueue_log)

        # Workers
        self.worker = WorkerManager(self)
        self.worker.log.connect(self._enqueue_log)
        self.worker.error.connect(self._show_error_dialog)
        self.worker.progress.connect(self._on_progress)
        self.worker.task_status.connect(self._on_task_status)
        self._doubao_worker: Optional[DoubaoImageWorker] = None

        # Log file
        self._log_path: Optional[Path] = None
        self._log_file: Optional[TextIO] = None
        self._log_pending = 0
        self._log_view_error_reported = False
        self._exec_poll_inflight = False
        self._exec_poll_failures = 0
        self._exec_poll_timer = QTimer(self)
        self._exec_poll_timer.setInterval(800)
        self._exec_poll_timer.timeout.connect(self._poll_execution_status)

        # UI
        self._build_ui()
        self._connect_signals()
        self._refresh_ports()
        self._load_config()

        self._enqueue_log("host", "WS63 激光主控终端 V2 已准备就绪")
        self._update_status_badges()

    def _build_ui(self) -> None:
        root = QWidget()
        root_layout = QVBoxLayout(root)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(0)

        # Status strip
        strip = QWidget()
        strip.setObjectName("statusStrip")
        strip_layout = QHBoxLayout(strip)
        strip.setFixedHeight(56)
        strip_layout.setContentsMargins(24, 8, 24, 8)
        strip_layout.setSpacing(12)

        self.badge_mode = StatusBadge("模式", "SLE")
        self.badge_tx = StatusBadge("TX串口", "关闭")
        self.badge_rx = StatusBadge("RX日志", "关闭")
        self.badge_focus = StatusBadge("调焦", "关闭")
        for b in (self.badge_mode, self.badge_tx, self.badge_rx, self.badge_focus):
            strip_layout.addWidget(b)
        strip_layout.addStretch(1)

        self.lbl_brand = QLabel("●  WS63 激光主控终端 V2")
        self.lbl_brand.setObjectName("headerBrand")
        strip_layout.addWidget(self.lbl_brand)

        root_layout.addWidget(strip)

        # Body: sidebar + pages
        body = QWidget()
        body_layout = QHBoxLayout(body)
        body_layout.setContentsMargins(0, 0, 0, 0)
        body_layout.setSpacing(0)

        self.sidebar = SidebarNav()
        body_layout.addWidget(self.sidebar)

        self.pages = QStackedWidget()
        self.page_conn = ConnectionPage()
        self.page_gcode = GcodePage()
        self.page_job = JobPage()
        self.page_logs = LogsPage()
        self.pages.addWidget(self.page_conn)
        self.pages.addWidget(self.page_gcode)
        self.pages.addWidget(self.page_job)
        self.pages.addWidget(self.page_logs)
        body_layout.addWidget(self.pages, 1)

        root_layout.addWidget(body, 1)
        self.setCentralWidget(root)

    def _connect_signals(self) -> None:
        # Sidebar navigation
        self.sidebar.page_changed.connect(self.pages.setCurrentIndex)

        # Connection page
        self.page_conn.connect_requested.connect(self._on_connect)
        self.page_conn.disconnect_requested.connect(self._on_disconnect)
        self.page_conn.tx_log_requested.connect(self._on_tx_log)
        self.page_conn.rx_log_requested.connect(self._on_rx_log)
        self.page_conn.stop_tx_log_requested.connect(self._on_stop_tx_log)
        self.page_conn.stop_rx_log_requested.connect(self._on_stop_rx_log)
        self.page_conn.refresh_requested.connect(self._refresh_ports)

        # G-code page
        self.page_gcode.gcode_loaded.connect(self._on_gcode_loaded)
        self.page_gcode.ai_generation_requested.connect(self._on_ai_generation_requested)

        # Job page
        self.page_job.upload_requested.connect(self._on_upload)
        self.page_job.upload_exec_requested.connect(self._on_upload_exec)
        self.page_job.exec_start_requested.connect(self._on_exec_start)
        self.page_job.exec_stop_requested.connect(self._on_exec_stop)
        self.page_job.abort_requested.connect(self._on_abort)
        self.page_job.outline_scan_requested.connect(self._on_outline_scan_requested)
        self.page_job.focus_on_requested.connect(self._on_focus_on)
        self.page_job.focus_off_requested.connect(self._on_focus_off)
        self.page_job.status_requested.connect(self._on_status)
        self.page_job.switch_wifi_requested.connect(self._on_switch_wifi)

        # Connection & Settings page
        self.page_conn.save_requested.connect(self._on_save_settings)

        # Event bus (for log routing from transport)
        self.event_bus.log_message.connect(self._on_log_message)
        self.focus_state_changed.connect(self._apply_focus_state)
        self.status_line_received.connect(self._parse_rx_line)

    def _enqueue_log(self, channel: str, message: str) -> None:
        self.event_bus.emit_log(channel, message)

    def _show_error_dialog(self, message: str) -> None:
        QMessageBox.warning(self, "任务错误", message)

    def _on_log_message(self, channel: str, message: str) -> None:
        # Protocol state is safety-critical and must not depend on log rendering.
        self._process_protocol_message(message)

        # Write to file
        if self._log_file:
            stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            kind_map = {"tx_log": "TX_LOG", "rx_log": "RX_LOG", "tx": "TX", "rx": "RX",
                        "status": "STATUS", "error": "ERROR", "host": "HOST"}
            kind = kind_map.get(channel, channel.upper())
            self._log_file.write(f"[{stamp}] {kind:8s} {message}\n")
            self._log_pending += 1
            if self._log_pending >= 40:
                self._log_file.flush()
                self._log_pending = 0

        # Presentation failures must never block RX state convergence.
        try:
            self.page_logs.append_log(channel, message)
        except Exception as exc:
            if not self._log_view_error_reported:
                self._log_view_error_reported = True
                print(f"[HOST_LOG_VIEW_ERROR] {type(exc).__name__}: {exc}")
                if self._log_file:
                    self._log_file.write(
                        f"[HOST_LOG_VIEW_ERROR] {type(exc).__name__}: {exc}\n"
                    )
                    self._log_file.flush()

    def _process_protocol_message(self, message: str) -> None:
        # @STATUS may arrive on the TX command port or either optional log port.
        self._parse_rx_line(message)

        m = RX_EXEC_DONE_RE.search(message)
        if m:
            done_job = int(m.group(1))
            tracked_job = self.state.rx_job_id
            if tracked_job > 0 and done_job != tracked_job:
                self._enqueue_log(
                    "status",
                    f"[EXEC_TRACK] ignore_done done_job={done_job} tracked_job={tracked_job}",
                )
                return
            self._enqueue_log(
                "status",
                f"[EXEC_TRACK] evidence=JOB_EXEC_DONE job={done_job} expected={int(self.state.execution_expected)}",
            )
            self._mark_execution_complete()

    def _parse_rx_line(self, message: str) -> None:
        status = parse_rx_status(message)
        if status is None:
            return
        previous_state = self.state.rx_state_code
        tracked_job = self.state.rx_job_id
        self.state.prev_rx_state_code = previous_state
        self.state.rx_state_code = status.state
        self.state.rx_job_id = status.job_id
        self.state.rx_received = status.received_size
        self.state.rx_job_total = status.job_total
        self.state.rx_cache_free = status.cache_free

        # Log state transitions
        if previous_state != status.state:
            self._enqueue_log("status", f"[RX_STATE] {previous_state} -> {status.state} job={status.job_id} rx={status.received_size}/{status.job_total}")

        if self.state.execution_expected:
            self._enqueue_log(
                "status",
                f"[EXEC_TRACK] evidence=STATUS prev={previous_state} state={status.state} "
                f"job={status.job_id} tracked_job={tracked_job} term={int(self.state.termination_requested)}",
            )

        same_job = tracked_job <= 0 or status.job_id in (0, tracked_job)
        completed_by_idle = (
            self.state.execution_expected
            and status.state == 0
            and same_job
            and not self.state.termination_requested
        )
        if completed_by_idle:
            self._enqueue_log(
                "status",
                f"[EXEC_TRACK] evidence=STATUS_IDLE job={status.job_id} previous={previous_state}",
            )
            self._mark_execution_complete()
            return
        self._update_monitor()
        if status.state == 3:
            self.state.execution_expected = True
            self.state.execution_complete = False
            self.page_job.set_execution_state(True)
            self._start_execution_status_poll()
        elif status.state in (5, 6):
            self.state.execution_expected = False
            self.page_job.set_execution_state(False)
            self._stop_execution_status_poll()

    def _mark_execution_complete(self) -> None:
        already_complete = self.state.execution_complete
        self._enqueue_log(
            "status",
            f"[EXEC_TRACK] complete job={self.state.rx_job_id} already={int(already_complete)}",
        )
        self.state.rx_state_code = 0
        self.state.execution_expected = False
        self.state.termination_requested = False
        self.state.execution_complete = True
        self._stop_execution_status_poll()
        self._update_monitor()
        self.page_job.set_execution_state(False, completed=True)
        if not already_complete:
            self.page_job.set_task_status("执行完成", -1)

    def _start_execution_status_poll(self) -> None:
        if self._exec_poll_timer.isActive() or not self._client_is_open():
            return
        self._exec_poll_failures = 0
        self._exec_poll_timer.start()
        self._enqueue_log("status", "[EXEC_POLL] started")

    def _stop_execution_status_poll(self) -> None:
        if self._exec_poll_timer.isActive():
            self._exec_poll_timer.stop()
            self._enqueue_log("status", "[EXEC_POLL] stopped")
        self._exec_poll_inflight = False

    def _poll_execution_status(self) -> None:
        if (not self.state.execution_expected or self.state.execution_complete or
                self.state.termination_requested):
            self._stop_execution_status_poll()
            return
        if self._exec_poll_inflight or not self._client_is_open():
            return

        self._exec_poll_inflight = True
        thread = threading.Thread(
            target=self._execution_status_poll_worker,
            name="exec-status-poll",
            daemon=True,
        )
        thread.start()

    def _execution_status_poll_worker(self) -> None:
        try:
            result = self.client.transact_status(2.0)
            self._exec_poll_failures = 0
            self.worker.log.emit(
                "status", f"[EXEC_POLL] STATUS {result.elapsed_ms}ms: {result.line}"
            )
            self.status_line_received.emit(result.line)
        except Exception as exc:
            self._exec_poll_failures += 1
            if self._exec_poll_failures in (1, 3, 10):
                self.worker.log.emit("error", f"[EXEC_POLL] 查询状态失败: {exc}")
        finally:
            self._exec_poll_inflight = False

    def _client_is_open(self) -> bool:
        is_open = getattr(self.client, "is_open", None)
        return bool(is_open()) if callable(is_open) else False

    def _update_status_badges(self) -> None:
        s = self.state
        tone_tx = "ok" if s.tx_state == LinkState.CONNECTED else "default"
        tone_rx = "ok" if s.rx_state == LinkState.CONNECTED else "default"
        tone_focus = "warn" if s.focus_active else "default"

        tx_map = {
            LinkState.CONNECTED: "已连接",
            LinkState.DISCONNECTED: "关闭",
            LinkState.CONNECTING: "连接中",
            LinkState.ERROR: "故障"
        }
        rx_map = {
            LinkState.CONNECTED: "已连接",
            LinkState.DISCONNECTED: "关闭",
            LinkState.CONNECTING: "连接中",
            LinkState.ERROR: "故障"
        }
        val_tx = tx_map.get(s.tx_state, s.tx_state.value)
        val_rx = rx_map.get(s.rx_state, s.rx_state.value)
        val_focus = "开启" if s.focus_active else "关闭"

        self.badge_mode.set_value(s.mode.value, "info")
        self.badge_tx.set_value(val_tx, tone_tx)
        self.badge_rx.set_value(val_rx, tone_rx)
        self.badge_focus.set_value(val_focus, tone_focus)

    def _update_monitor(self) -> None:
        s = self.state
        self.page_job.update_state(
            rx_state=s.rx_state_name,
            rx_state_code=s.rx_state_code,
            job_id=s.rx_job_id,
            received=s.rx_received,
            total=s.rx_job_total,
            cache_free=s.rx_cache_free,
            focus="ON" if s.focus_active else "OFF",
            tx_link=s.tx_state.value,
            rx_link=s.rx_state.value,
        )

    def _on_progress(self, text: str) -> None:
        self.page_job.set_progress_text(text)

    def _on_task_status(self, text: str, pct: float) -> None:
        execution_start = text.startswith(
            ("执行中", "执行已启动", "预缓冲执行中", "预缓冲完成，启动执行")
        )
        if execution_start and self.state.execution_complete:
            self._enqueue_log(
                "status",
                f"[EXEC_TRACK] ignore_stale_start job={self.state.rx_job_id} text={text}",
            )
            self.page_job.set_task_status("执行完成", 100)
            self.page_job.set_execution_state(False, completed=True)
            return

        self.page_job.set_task_status(text, pct)
        if execution_start:
            was_expected = self.state.execution_expected
            self.state.execution_expected = True
            self.state.termination_requested = False
            self.state.execution_complete = False
            self._update_monitor()
            self.page_job.set_execution_state(True)
            self._start_execution_status_poll()
            if not was_expected:
                self._enqueue_log(
                    "status",
                    f"[EXEC_TRACK] start job={self.state.rx_job_id} text={text}",
                )
        elif text in ("已停止", "已放弃"):
            self.state.execution_expected = False
            self.state.termination_requested = True
            self.state.execution_complete = False
            self._stop_execution_status_poll()
            self.page_job.set_execution_state(False)
        elif text in ("停止命令失败", "放弃命令失败"):
            self.state.termination_requested = False
        elif text == "执行失败":
            self.state.execution_expected = False
            self.state.execution_complete = False
            self._stop_execution_status_poll()
            self.page_job.set_execution_state(False)
        if text.startswith("上传完成"):
            self.state.rx_state_code = 2
            self.state.rx_received = self.state.rx_job_total
            self._update_monitor()
        elif 0 <= pct <= 100 and any(tag in text for tag in ("上传", "预缓冲", "继续")):
            self.state.rx_state_code = 1
            self.state.rx_received = int(self.state.rx_job_total * pct / 100)
            self._update_monitor()

    # ---- Connection ----

    def _refresh_ports(self) -> None:
        ports = available_ports()
        self.page_conn.set_ports(ports)
        self._enqueue_log("host", f"检测到 {len(ports)} 个串口")

    def _load_config(self) -> None:
        self.page_conn.load_config(self.config)
        self.page_job.load_config(self.config)

    def _on_connect(self, port: str, display: str, baud: int) -> None:
        try:
            self.client.open(port, baud)
            self.state.tx_state = LinkState.CONNECTED
            self.page_conn.set_connected(True)
            self._start_log_file()
            self._update_status_badges()
            self._update_monitor()
        except Exception as exc:
            self.state.tx_state = LinkState.ERROR
            self._update_status_badges()
            self._update_monitor()
            self._enqueue_log("error", f"连接失败: {exc}")

    def _on_disconnect(self) -> None:
        if self.worker.is_busy():
            self._enqueue_log("error", "任务操作进行中，拒绝断开命令串口")
            return
        if not self._focus_off_before_disconnect():
            return
        self._stop_execution_status_poll()
        self.client.close()
        self.state.tx_state = LinkState.DISCONNECTED
        self.page_conn.set_connected(False)
        self._update_status_badges()
        self._update_monitor()

    def _on_tx_log(self, port: str, baud: int) -> None:
        try:
            self.tx_monitor.open(port, baud)
            self.page_conn.set_tx_log_active(True)
        except Exception as exc:
            self._enqueue_log("error", f"TX 日志监听失败: {exc}")

    def _on_rx_log(self, port: str, baud: int) -> None:
        try:
            self.rx_monitor.open(port, baud)
            self.state.rx_state = LinkState.CONNECTED
            self.page_conn.set_rx_log_active(True)
            self._update_status_badges()
            self._update_monitor()
        except Exception as exc:
            self.state.rx_state = LinkState.ERROR
            self._update_status_badges()
            self._update_monitor()
            self._enqueue_log("error", f"RX 日志监听失败: {exc}")

    def _on_stop_tx_log(self) -> None:
        self.tx_monitor.close()
        self.page_conn.set_tx_log_active(False)

    def _on_stop_rx_log(self) -> None:
        self.rx_monitor.close()
        self.state.rx_state = LinkState.DISCONNECTED
        self.page_conn.set_rx_log_active(False)
        self._update_status_badges()
        self._update_monitor()

    def _start_log_file(self) -> None:
        if self._log_path:
            return
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self._log_path = Path(__file__).resolve().parent.parent / "logs" / f"session_{stamp}.log"
        self._log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log_file = self._log_path.open("a", encoding="utf-8")
        self._enqueue_log("host", f"日志文件: {self._log_path}")

    # ---- G-code ----

    def _on_gcode_loaded(self, path: str, data: bytes) -> None:
        self.state.gcode_path = path
        self.state.gcode_bytes = data
        if data:
            self._enqueue_log("host", f"已加载: {path} ({len(data)} 字节)")
        else:
            self._enqueue_log("host", "G-code 编辑器已清空")

    def _on_ai_generation_requested(
        self, prompt: str, size: str, watermark: bool, output_dir: str
    ) -> None:
        if not os.environ.get("ARK_API_KEY", "").strip():
            message = "未检测到 ARK_API_KEY，请先在系统环境变量中配置火山方舟 API Key"
            self.page_gcode.image_status.setText(message)
            self.page_gcode.update_ui_generating_state(False)
            self._enqueue_log("error", message)
            return
        if self._doubao_worker is not None and self._doubao_worker.isRunning():
            self.page_gcode.image_status.setText("图像生成任务进行中，请等待完成")
            return

        worker = DoubaoImageWorker(prompt, size, watermark, output_dir, self)
        worker.log.connect(self._enqueue_log)
        worker.status.connect(self.page_gcode.image_status.setText)
        worker.image_ready.connect(self._on_ai_image_ready)
        worker.error.connect(self._on_ai_image_error)
        worker.finished.connect(self._on_ai_image_finished)
        self._doubao_worker = worker
        worker.start()

    def _on_ai_image_ready(self, path: str) -> None:
        image = QImage(path)
        if image.isNull():
            self._on_ai_image_error(f"生成图片已保存但无法预览: {path}")
            return
        self.page_gcode.set_image(path, image)
        self.page_gcode.image_status.setText(f"豆包生图完成，已保存: {path}")

    def _on_ai_image_error(self, message: str) -> None:
        self.page_gcode.image_status.setText(message)
        self._enqueue_log("error", f"豆包生图失败: {message}")

    def _on_ai_image_finished(self) -> None:
        self.page_gcode.update_ui_generating_state(False)
        self._doubao_worker = None

    def _on_outline_scan_requested(self) -> None:
        payload = self.page_gcode.build_outline_scan_payload()
        if payload is None:
            self._enqueue_log("error", "当前 G-code 没有可扫描的开光外框")
            return
        path, data = payload
        if not self._client_is_open():
            self._enqueue_log("error", "TX 命令串口未连接，无法扫描外框")
            return
        if self.worker.is_busy():
            self._enqueue_log("error", "任务进行中")
            return
        job_id = self.page_job.get_job_id()
        self.state.gcode_path = path
        self._enqueue_log("status", f"开始扫描外框: {path} ({len(data)} 字节，两圈)")
        self._begin_job_tracking(data, job_id)
        self._run_job_worker(
            self._upload_exec_worker,
            data,
            job_id,
            self.page_job.get_timeout(),
            0,
        )

    # ---- Job operations ----

    def _run_job_worker(self, fn, *args) -> None:
        if self.worker.is_busy():
            self._enqueue_log("error", "任务进行中")
            return
        self.worker.run(fn, *args)

    def _on_upload(self) -> None:
        gcode = self._get_gcode()
        if not gcode:
            return
        if self.worker.is_busy():
            self._enqueue_log("error", "任务进行中")
            return
        self._begin_job_tracking(gcode, self.page_job.get_job_id())
        self._run_job_worker(
            self._upload_worker,
            gcode,
            self.page_job.get_job_id(),
            self.page_job.get_timeout(),
        )

    def _on_upload_exec(self) -> None:
        gcode = self._get_gcode()
        if not gcode:
            return
        if self.worker.is_busy():
            self._enqueue_log("error", "任务进行中")
            return
        self._begin_job_tracking(gcode, self.page_job.get_job_id())
        self._run_job_worker(
            self._upload_exec_worker,
            gcode,
            self.page_job.get_job_id(),
            self.page_job.get_timeout(),
            self.page_job.get_preroll(),
        )

    def _upload_worker(
        self, gcode: bytes, job_id: int, timeout: float
    ) -> None:
        self._focus_off_before_job()
        crc = crc16_ccitt(gcode)
        self.worker.log.emit(
            "status", f"开始仅上传 job={job_id} size={len(gcode)} crc=0x{crc:04x} preroll=disabled"
        )
        self.worker.task_status.emit("上传中...", 0)
        self.client.upload_job(
            job_id, gcode, timeout,
            progress_cb=lambda t: self.worker.progress.emit(t),
            status_cb=lambda t, p: self.worker.task_status.emit(t, p),
            preroll_bytes=0,
            start_on_preroll=False,
        )
        self.worker.task_status.emit("上传完成，RX 任务已就绪", 100)

    def _upload_exec_worker(
        self, gcode: bytes, job_id: int, timeout: float, preroll: int
    ) -> None:
        self._focus_off_before_job()
        crc = crc16_ccitt(gcode)
        total = len(gcode)
        effective_preroll = preroll
        if total > JOB_MAX_SIZE and (effective_preroll <= 0 or effective_preroll >= total):
            effective_preroll = min(JOB_PREROLL_BYTES, total - 1)
            self.worker.log.emit(
                "status",
                f"[EXEC_FLOW] 大文件超过64K，强制启用 preroll={effective_preroll}",
            )
        self.worker.log.emit("status", f"[EXEC_FLOW] 开始上传并执行 job={job_id} size={total} crc=0x{crc:04x} preroll={effective_preroll}")

        # Determine small-file vs large-file path
        use_preroll = effective_preroll > 0 and total > effective_preroll
        if effective_preroll > 0 and total <= effective_preroll:
            self.worker.log.emit("status", f"[EXEC_FLOW] 小文件 ({total} <= {effective_preroll}), 使用 normal upload 路径")
        self.worker.log.emit("status", f"[EXEC_FLOW] use_preroll={use_preroll} path={'preroll' if use_preroll else 'normal'}")

        # Reset execution state. Completion is received asynchronously from RX.
        self.state.execution_complete = False
        self.state.execution_expected = False
        self.state.termination_requested = False

        try:
            # Upload phase
            self.worker.log.emit("status", f"[EXEC_FLOW] 开始上传阶段")
            self.worker.task_status.emit("上传中...", 0)
            self.client.upload_job(
                job_id, gcode, timeout,
                progress_cb=lambda t: self.worker.progress.emit(t),
                status_cb=lambda t, p: self.worker.task_status.emit(t, p),
                preroll_bytes=effective_preroll if use_preroll else 0,
                start_on_preroll=use_preroll,
                enforce_job_size_limit=False,
            )
            self.worker.log.emit("status", f"[EXEC_FLOW] 上传阶段完成")

            # Execution phase
            if use_preroll:
                # Large file: preroll path already started execution during upload
                self.worker.log.emit("status", f"[EXEC_FLOW] 大文件 preroll 路径，执行已在上传中启动")
                self.worker.task_status.emit("预缓冲执行中，等待 RX 完成", -1)
            else:
                # Small file: send EXEC_START after upload complete
                self.worker.log.emit("status", f"[EXEC_FLOW] 小文件 normal 路径，发送 EXEC_START")
                self.worker.task_status.emit("执行中...", -1)
                self.client.send_control(f"@EXEC_START {job_id}", "@ACK type=16", timeout)
                self.worker.log.emit("status", f"[EXEC_FLOW] EXEC_START 已发送，等待 RX 完成")
                self.worker.task_status.emit("执行已启动，等待 RX 完成", -1)

            self.worker.task_status.emit("执行中，等待 RX 完成", -1)
            self.worker.log.emit("status", "[EXEC_FLOW] 执行已启动，串口控制 worker 已释放")
        except Exception as exc:
            self.worker.log.emit("error", f"[EXEC_FLOW] 异常: {exc}")
            self.worker.task_status.emit("执行失败", -1)
            raise

    def _get_gcode(self) -> bytes:
        gcode = self.page_gcode.get_gcode_bytes()
        if not gcode:
            self.worker.log.emit("error", "G-code 内容为空")
            return b""
        try:
            payload, diag = prepare_gcode_for_upload(
                gcode.decode("utf-8", errors="replace"),
                source=self.state.gcode_path or "editor",
                preroll_bytes=4096,
            )
            # Log diagnostic
            self.worker.log.emit("status", format_diagnostic(diag))
            return payload
        except RuntimeError as exc:
            self.worker.log.emit("error", str(exc))
            return b""

    def _begin_job_tracking(self, gcode: bytes, job_id: int) -> None:
        self.state.prev_rx_state_code = self.state.rx_state_code
        self.state.rx_state_code = 1
        self.state.rx_job_id = job_id
        self.state.rx_received = 0
        self.state.rx_job_total = len(gcode)
        self.state.execution_expected = False
        self.state.termination_requested = False
        self.state.execution_complete = False
        self.page_job.set_execution_state(False)
        self._update_monitor()
        self._enqueue_log(
            "status", f"任务上传基线: total_bytes={len(gcode)}"
        )

    def _on_exec_start(self) -> None:
        job_id = self.page_job.get_job_id()
        timeout = self.page_job.get_timeout()
        self.state.execution_complete = False
        self.state.termination_requested = False

        def _work():
            self.client.send_control(f"@EXEC_START {job_id}", "@ACK type=16", timeout)
            self.worker.task_status.emit("执行中，等待 RX 完成", -1)
        self._run_job_worker(_work)

    def _on_exec_stop(self) -> None:
        if self.worker.is_busy():
            self._enqueue_log("error", "任务进行中")
            return
        timeout = self.page_job.get_timeout()
        self.state.termination_requested = True
        self.state.execution_complete = False

        def _work():
            try:
                self.client.send_control("@EXEC_STOP", "@ACK type=19", timeout)
            except Exception:
                self.worker.task_status.emit("停止命令失败", -1)
                raise
            self.worker.task_status.emit("已停止", -1)
        self._run_job_worker(_work)

    def _on_abort(self) -> None:
        if self.worker.is_busy():
            self._enqueue_log("error", "任务进行中")
            return
        timeout = self.page_job.get_timeout()
        self.state.termination_requested = True
        self.state.execution_complete = False

        def _work():
            try:
                self.client.send_control("@ABORT", "@ACK type=4", timeout)
            except Exception:
                self.worker.task_status.emit("放弃命令失败", -1)
                raise
            self.worker.task_status.emit("已放弃", -1)
        self._run_job_worker(_work)

    def _on_focus_on(self, power: int) -> None:
        def _work():
            self.client.send_control(f"@FOCUS_ON S{power}", "@OK focus_on", 5.0)
            self.focus_state_changed.emit(True)
            self.worker.log.emit("status", f"FOCUS_ON S={power}")
        self._run_job_worker(_work)

    def _on_focus_off(self) -> None:
        def _work():
            self.client.send_control("@FOCUS_OFF", "@OK focus_off", 5.0)
            self.focus_state_changed.emit(False)
            self.worker.log.emit("status", "FOCUS_OFF")
        self._run_job_worker(_work)

    def _focus_off_before_job(self) -> None:
        if not self.state.focus_active:
            return
        self.worker.log.emit("status", "上传前自动关闭调焦光")
        self.client.send_control("@FOCUS_OFF", "@OK focus_off", 5.0)
        self.focus_state_changed.emit(False)

    def _focus_off_before_disconnect(self) -> bool:
        if not self.state.focus_active:
            return True
        if self.worker.is_busy():
            self._enqueue_log("error", "调焦光状态未确认关闭，当前任务忙，拒绝断开")
            return False
        try:
            self.client.send_control("@FOCUS_OFF", "@OK focus_off", 5.0)
        except Exception as exc:
            self._enqueue_log("error", f"调焦光关闭失败，拒绝断开: {exc}")
            return False
        self.focus_state_changed.emit(False)
        self._enqueue_log("status", "断开前已关闭调焦光")
        return True

    def _on_status(self) -> None:
        def _work():
            result = self.client.transact_status(8.0)
            self.worker.log.emit("status", f"STATUS {result.elapsed_ms}ms: {result.line}")
            self.status_line_received.emit(result.line)
        self._run_job_worker(_work)

    def _apply_focus_state(self, active: bool) -> None:
        self.state.focus_active = active
        self.page_job.set_focus_state(active)
        self._update_status_badges()
        self._update_monitor()

    def _on_switch_wifi(self) -> None:
        def _work():
            self.client.send_control("@RX MODE=GRBL", "@OK route_switch_accepted", 8.0)
            self.worker.task_status.emit("Wi-Fi 切换请求已接受，等待外部验证", 100)
            self.worker.log.emit(
                "status", "ROUTE_SWITCH accepted；请用 LaserGRBL 验证 Wi-Fi 路由"
            )
        self._run_job_worker(_work)

    def _on_save_settings(self, config: HostConfig) -> None:
        self.config_store.save(config)
        self.config = config
        self.page_conn.load_config(config)
        self.page_job.load_config(config)
        self._enqueue_log("host", f"设置已保存: {self.config_store.path}")

    def closeEvent(self, event) -> None:
        if self._doubao_worker is not None and self._doubao_worker.isRunning():
            self.page_gcode.image_status.setText("豆包生图仍在进行中，请等待完成后再退出")
            self._enqueue_log("error", "豆包生图仍在进行中，拒绝退出上位机")
            event.ignore()
            return
        if self.worker.is_busy():
            self._enqueue_log("error", "任务操作进行中，拒绝退出上位机")
            event.ignore()
            return
        if not self._focus_off_before_disconnect():
            event.ignore()
            return
        self.client.close()
        self._stop_execution_status_poll()
        self.tx_monitor.close()
        self.rx_monitor.close()
        if self._log_file:
            self._log_file.flush()
            self._log_file.close()
        super().closeEvent(event)
