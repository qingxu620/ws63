"""
Main window — integrates all pages, manages transport, workers, and state.
"""
from __future__ import annotations

import os
import re
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
from app.paths import session_log_dir
from app.state_models import AppState, ConnectionMode, LinkState
from transports.sle_tx_transport import (
    SleJobSerialClient, SerialLogMonitor, UploadInterrupted, available_ports, crc16_ccitt,
    parse_rx_status, prepare_gcode_for_rx, RX_EXEC_DONE_RE,
    JOB_DATA_CHUNK_SIZE, JOB_EXEC_PREROLL_CACHE_HEADROOM_BYTES,
    JOB_MAX_SIZE, JOB_PREROLL_BYTES, EXEC_START_ACK_TIMEOUT_MIN_S,
    clamp_stream_preroll_request, plan_safe_preroll,
)
from workers.doubao_image_worker import DoubaoImageWorker
from workers.serial_worker import WorkerManager
from ui.widgets.status_badge import StatusBadge
from ui.widgets.sidebar_nav import SidebarNav
from ui.pages.connection_page import ConnectionPage
from ui.pages.gcode_page import GcodePage
from ui.pages.job_page import JobPage
from ui.pages.logs_page import LogsPage

EXEC_STATUS_POLL_INTERVAL_MS = 5000
EXEC_STATUS_POLL_TIMEOUT_S = 6.0
MODE_STATUS_SYNC_RETRY_MS = 100
CANCELLED_TASK_HOLD_MS = 2000

LINK_EVENT_RE = re.compile(
    r"@LINK\s+peer=(RX|SCREEN)\s+state=(CONNECTED|DISCONNECTED|CONNECTING|ERROR)\b",
    re.IGNORECASE,
)
MODE_EVENT_RE = re.compile(r"@MODE\s+([^\r\n]+)", re.IGNORECASE)
PROTOCOL_FIELD_RE = re.compile(r"\b([A-Z_]+)=([^\s]+)", re.IGNORECASE)


class MainWindow(QMainWindow):
    focus_state_changed = Signal(bool)
    status_line_received = Signal(str)
    mode_request_acknowledged = Signal(str)
    mode_request_failed = Signal(str, str)

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
        self._cancelled_job_id = 0
        self._exec_poll_timer = QTimer(self)
        self._exec_poll_timer.setInterval(EXEC_STATUS_POLL_INTERVAL_MS)
        self._exec_poll_timer.timeout.connect(self._poll_execution_status)
        self._mode_status_sync_pending = False
        self._mode_status_sync_timer = QTimer(self)
        self._mode_status_sync_timer.setSingleShot(True)
        self._mode_status_sync_timer.setInterval(MODE_STATUS_SYNC_RETRY_MS)
        self._mode_status_sync_timer.timeout.connect(self._try_mode_status_sync)
        self._cancelled_task_timer = QTimer(self)
        self._cancelled_task_timer.setSingleShot(True)
        self._cancelled_task_timer.setInterval(CANCELLED_TASK_HOLD_MS)
        self._cancelled_task_timer.timeout.connect(self._return_cancelled_task_to_idle)

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
        self.page_gcode.ai_generation_cancel_requested.connect(
            self._on_ai_generation_cancel_requested
        )

        # Job page
        self.page_job.upload_requested.connect(self._on_upload)
        self.page_job.upload_exec_requested.connect(self._on_upload_exec)
        self.page_job.exec_start_requested.connect(self._on_exec_start)
        self.page_job.exec_stop_requested.connect(self._on_exec_stop)
        self.page_job.exec_resume_requested.connect(self._on_exec_resume)
        self.page_job.abort_requested.connect(self._on_abort)
        self.page_job.outline_scan_requested.connect(self._on_outline_scan_requested)
        self.page_job.focus_on_requested.connect(self._on_focus_on)
        self.page_job.focus_off_requested.connect(self._on_focus_off)
        self.page_job.status_requested.connect(self._on_status)
        self.page_job.mode_requested.connect(self._on_mode_selected)

        # Connection & Settings page
        self.page_conn.save_requested.connect(self._on_save_settings)

        # Event bus (for log routing from transport)
        self.event_bus.log_message.connect(self._on_log_message)
        self.focus_state_changed.connect(self._apply_focus_state)
        self.status_line_received.connect(self._parse_rx_line)
        self.mode_request_acknowledged.connect(self._on_mode_request_acknowledged)
        self.mode_request_failed.connect(self._on_mode_request_failed)

    def _enqueue_log(self, channel: str, message: str) -> None:
        self.event_bus.emit_log(channel, message)

    def _show_error_dialog(self, message: str) -> None:
        QMessageBox.warning(self, "任务错误", message)

    def _write_log_file(self, text: str, *, flush: bool = False) -> None:
        log_file = self._log_file
        if log_file is None or log_file.closed:
            return
        try:
            log_file.write(text)
            if flush:
                log_file.flush()
        except ValueError:
            # Late queued log messages can arrive after closeEvent has closed
            # the file. Logging must never crash the UI.
            self._log_file = None

    def _close_log_file(self) -> None:
        log_file = self._log_file
        self._log_file = None
        if log_file is None or log_file.closed:
            return
        log_file.flush()
        log_file.close()

    def _on_log_message(self, channel: str, message: str) -> None:
        # Protocol state is safety-critical and must not depend on log rendering.
        self._process_protocol_message(message)
        if channel == "protocol":
            return

        # Write to file
        if self._log_file is not None and not self._log_file.closed:
            stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            kind_map = {"tx_log": "TX_LOG", "rx_log": "RX_LOG", "tx": "TX", "rx": "RX",
                        "status": "STATUS", "error": "ERROR", "host": "HOST"}
            kind = kind_map.get(channel, channel.upper())
            self._write_log_file(f"[{stamp}] {kind:8s} {message}\n")
            self._log_pending += 1
            if self._log_pending >= 40:
                self._write_log_file("", flush=True)
                self._log_pending = 0

        # Presentation failures must never block RX state convergence.
        try:
            self.page_logs.append_log(channel, message)
        except Exception as exc:
            if not self._log_view_error_reported:
                self._log_view_error_reported = True
                print(f"[HOST_LOG_VIEW_ERROR] {type(exc).__name__}: {exc}")
                self._write_log_file(
                    f"[HOST_LOG_VIEW_ERROR] {type(exc).__name__}: {exc}\n",
                    flush=True,
                )

    def _process_protocol_message(self, message: str) -> None:
        self._process_mode_protocol_message(message)

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

    @staticmethod
    def _protocol_fields(text: str) -> dict[str, str]:
        return {
            key.upper(): value.rstrip(",;")
            for key, value in PROTOCOL_FIELD_RE.findall(text)
        }

    @staticmethod
    def _connection_mode(value: str) -> Optional[ConnectionMode]:
        normalized = value.strip().upper()
        if normalized == "SLE":
            return ConnectionMode.SLE_VIA_TX
        if normalized in ("WIFI", "WI-FI", "LEGACY_WIFI"):
            return ConnectionMode.WIFI_TCP
        return None

    def _process_mode_protocol_message(self, message: str) -> None:
        link_match = LINK_EVENT_RE.search(message)
        if link_match is not None:
            peer = link_match.group(1).upper()
            link_state = LinkState(link_match.group(2).upper())
            if peer == "RX":
                self.state.sle_rx_state = link_state
                if (link_state == LinkState.DISCONNECTED and
                        self.state.mode_target == ConnectionMode.SLE_VIA_TX and
                        self.state.mode_transition == "ACTIVE"):
                    self.state.mode_transition = "CONNECTING"
            else:
                self.state.screen_state = link_state
            self._refresh_mode_ui()

        mode_match = MODE_EVENT_RE.search(message)
        if mode_match is not None:
            fields = self._protocol_fields(mode_match.group(1))
            active_mode = self._connection_mode(fields.get("ACTIVE", ""))
            target_mode = self._connection_mode(fields.get("TARGET", ""))
            transition = fields.get("STATE", "").upper()

            if active_mode == ConnectionMode.SLE_VIA_TX:
                was_confirmed = (
                    self.state.mode == ConnectionMode.SLE_VIA_TX
                    and self.state.mode_target == ConnectionMode.SLE_VIA_TX
                    and self.state.mode_transition == "ACTIVE"
                    and self.state.sle_rx_state == LinkState.CONNECTED
                )
                self.state.mode = ConnectionMode.SLE_VIA_TX
                self.state.mode_target = ConnectionMode.SLE_VIA_TX
                self.state.mode_transition = "ACTIVE"
                self.state.sle_rx_state = (
                    LinkState.CONNECTED
                    if fields.get("RX", "").lower() == "ready"
                    else self.state.sle_rx_state
                )
                screen = fields.get("SCREEN", "").lower()
                if screen == "ready":
                    self.state.screen_state = LinkState.CONNECTED
                elif screen == "connecting":
                    self.state.screen_state = LinkState.CONNECTING
                self._refresh_mode_ui()
                if not was_confirmed:
                    self._schedule_mode_status_sync()
            elif target_mode is not None and transition:
                self.state.mode_target = target_mode
                self.state.mode_transition = transition
                if fields.get("RX", "").lower() == "connecting":
                    self.state.sle_rx_state = LinkState.CONNECTING
                elif fields.get("RX", "").lower() == "disconnected":
                    self.state.sle_rx_state = LinkState.DISCONNECTED
                screen = fields.get("SCREEN", "").lower()
                if screen == "ready":
                    self.state.screen_state = LinkState.CONNECTED
                elif screen == "connecting":
                    self.state.screen_state = LinkState.CONNECTING
                self._refresh_mode_ui(reason=fields.get("REASON", ""))

        # Compatibility with the route-switch response used before @MODE events.
        if "@OK route_switch_accepted" in message:
            self.state.mode_target = ConnectionMode.WIFI_TCP
            self.state.mode_transition = "PENDING"
            self._refresh_mode_ui()
        elif "@ERR route_switch_failed" in message:
            self.state.mode_target = ConnectionMode.WIFI_TCP
            self.state.mode_transition = "FAILED"
            self._refresh_mode_ui(reason="route_switch_failed")

    @staticmethod
    def _link_text(state: LinkState) -> str:
        return {
            LinkState.CONNECTED: "已连接",
            LinkState.DISCONNECTED: "未连接",
            LinkState.CONNECTING: "连接中",
            LinkState.ERROR: "故障",
        }.get(state, state.value)

    def _refresh_mode_ui(self, *, reason: str = "") -> None:
        target = self.state.mode_target
        transition = self.state.mode_transition
        rx_text = self._link_text(self.state.sle_rx_state)
        screen_text = self._link_text(self.state.screen_state)

        if transition == "FAILED":
            status = f"{target.value} 切换失败"
            if reason:
                status += f"：{reason}"
            tone = "error"
            badge_value = f"{target.value}失败"
            badge_tone = "error"
        elif target == ConnectionMode.WIFI_TCP:
            if transition == "EXTERNAL_VERIFY":
                status = "Wi-Fi 路由已脱离 SLE，请在 LaserGRBL 侧验证"
            elif transition == "REQUESTING":
                status = "正在请求切换至 Wi-Fi / LaserGRBL"
            else:
                status = "Wi-Fi 切换请求已接受，等待 LaserGRBL 外部验证"
            tone = "pending"
            badge_value = "WiFi待验证"
            badge_tone = "warn"
        elif transition == "ACTIVE" and self.state.sle_rx_state == LinkState.CONNECTED:
            status = f"SLE 已生效 · RX{rx_text} · Screen{screen_text}"
            tone = "ok"
            badge_value = "SLE"
            badge_tone = "ok"
        elif transition == "REQUESTING":
            status = "正在请求 TX 恢复 RX 与 Screen 星闪连接"
            tone = "pending"
            badge_value = "SLE请求中"
            badge_tone = "warn"
        elif transition == "CONNECTING":
            status = f"SLE 连接中 · RX{rx_text} · Screen{screen_text}"
            tone = "pending"
            badge_value = "SLE连接中"
            badge_tone = "warn"
        else:
            status = f"SLE 状态待 TX 确认 · RX{rx_text} · Screen{screen_text}"
            tone = "default"
            badge_value = self.state.mode.value
            badge_tone = "info"

        self.page_job.set_mode_selection(target.value, status, tone)
        sle_controls_enabled = (
            target == ConnectionMode.SLE_VIA_TX
            and transition not in ("REQUESTING", "CONNECTING", "FAILED")
        )
        self.page_job.set_sle_controls_enabled(sle_controls_enabled)
        self.badge_mode.set_value(badge_value, badge_tone)

    def _parse_rx_line(self, message: str) -> None:
        status = parse_rx_status(message)
        if status is None:
            return
        previous_state = self.state.rx_state_code
        tracked_job = self.state.rx_job_id
        same_job_before_update = tracked_job <= 0 or status.job_id in (0, tracked_job)
        cancelled_status = (
            self._cancelled_job_id > 0
            and status.job_id in (0, self._cancelled_job_id)
        )
        if status.state == 3 and (self.state.termination_requested or cancelled_status):
            self._enqueue_log(
                "status",
                f"[EXEC_TRACK] ignore_post_cancel_executing job={status.job_id} "
                f"tracked_job={tracked_job}",
            )
            return
        if self.state.execution_complete and status.state == 3 and same_job_before_update:
            self._enqueue_log(
                "status",
                f"[EXEC_TRACK] ignore_stale_status state={status.state} "
                f"job={status.job_id} tracked_job={tracked_job}",
            )
            return
        self.state.prev_rx_state_code = previous_state
        same_active_job = tracked_job > 0 and status.job_id in (0, tracked_job)
        self.state.rx_state_code = status.state
        self.state.rx_job_id = status.job_id
        self.state.rx_received = (
            max(self.state.rx_received, status.received_size)
            if same_active_job else status.received_size
        )
        self.state.rx_job_total = max(self.state.rx_job_total, status.job_total) if same_active_job else status.job_total
        self.state.rx_cache_free = status.cache_free
        self.state.rx_executed_lines = (
            max(self.state.rx_executed_lines, status.executed_lines)
            if same_active_job else status.executed_lines
        )
        self.state.rx_completed_lines = (
            max(self.state.rx_completed_lines, status.completed_lines)
            if same_active_job else status.completed_lines
        )
        self.state.rx_total_lines = (
            max(self.state.rx_total_lines, status.total_lines)
            if same_active_job else status.total_lines
        )

        # Log state transitions
        if previous_state != status.state:
            self._enqueue_log("status", f"[RX_STATE] {previous_state} -> {status.state} job={status.job_id} rx={status.received_size}/{status.job_total}")

        if self.state.execution_expected and previous_state != status.state:
            self._enqueue_log(
                "status",
                f"[EXEC_TRACK] evidence=STATUS prev={previous_state} state={status.state} "
                f"job={status.job_id} tracked_job={tracked_job} term={int(self.state.termination_requested)}",
            )

        completed_by_idle = (
            self.state.execution_expected
            and status.state == 0
            and same_job_before_update
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
        elif status.state == 4:
            self.state.execution_expected = True
            self.state.termination_requested = True
            self.state.execution_complete = False
            self.page_job.set_execution_state(False)
            self.page_job.set_task_status("已暂停", -1)
            self._stop_execution_status_poll()
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
            try_status = getattr(self.client, "try_transact_status", None)
            result = (
                try_status(EXEC_STATUS_POLL_TIMEOUT_S)
                if callable(try_status)
                else self.client.transact_status(EXEC_STATUS_POLL_TIMEOUT_S)
            )
            if result is None:
                return
            if (not self.state.execution_expected or self.state.execution_complete or
                    self.state.termination_requested or not self._exec_poll_timer.isActive()):
                return
            self._exec_poll_failures = 0
            self.status_line_received.emit(result.line)
        except Exception as exc:
            if (not self.state.execution_expected or self.state.execution_complete or
                    self.state.termination_requested or not self._exec_poll_timer.isActive()):
                return
            self._exec_poll_failures += 1
            if isinstance(exc, TimeoutError):
                if self._exec_poll_failures in (1, 3, 10):
                    self.worker.log.emit("status", f"[EXEC_POLL] 状态查询延迟: {exc}")
            elif self._exec_poll_failures in (1, 3, 10):
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

        self._refresh_mode_ui()
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
            completed_lines=s.rx_completed_lines,
            total_lines=s.rx_total_lines,
            focus="ON" if s.focus_active else "OFF",
            tx_link=s.tx_state.value,
            rx_link=s.rx_state.value,
        )

    def _on_progress(self, text: str) -> None:
        self.page_job.set_progress_text(text)

    def _on_task_status(self, text: str, pct: float) -> None:
        execution_start = text.startswith(
            ("执行中", "执行已启动", "预缓冲执行中", "预缓冲完成，启动执行", "RX 已自动启动")
        )
        if execution_start and self._cancelled_job_id > 0:
            self._enqueue_log(
                "status",
                f"[EXEC_TRACK] ignore_post_cancel_start job={self._cancelled_job_id} text={text}",
            )
            return
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
            self._cancelled_task_timer.stop()
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
        elif text in ("已停止", "已暂停", "已放弃", "已取消并清空"):
            self.state.execution_expected = False
            self.state.termination_requested = True
            self.state.execution_complete = False
            self._stop_execution_status_poll()
            self.page_job.set_execution_state(False)
            if text in ("已放弃", "已取消并清空"):
                self._cancelled_job_id = self.state.rx_job_id
                self.state.prev_rx_state_code = self.state.rx_state_code
                self.state.rx_state_code = 5
                self._update_monitor()
                self.page_job.show_cancelled_state()
                self._cancelled_task_timer.start()
        elif text in ("停止命令失败", "放弃命令失败", "取消命令失败"):
            self._cancelled_task_timer.stop()
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
            if not execution_start and self.state.rx_state_code != 3:
                self.state.rx_state_code = 1
            self.state.rx_received = max(
                self.state.rx_received,
                int(self.state.rx_job_total * pct / 100),
            )
            self._update_monitor()

    def _return_cancelled_task_to_idle(self) -> None:
        self._cancelled_task_timer.stop()
        if not self.state.termination_requested or self.state.execution_expected:
            return

        self.state.prev_rx_state_code = self.state.rx_state_code
        self.state.rx_state_code = 0
        self.state.rx_job_id = 0
        self.state.rx_received = 0
        self.state.rx_job_total = 0
        self.state.rx_cache_free = JOB_MAX_SIZE
        self.state.rx_executed_lines = 0
        self.state.rx_completed_lines = 0
        self.state.rx_total_lines = 0
        self.state.termination_requested = False
        self.state.execution_complete = False
        self._update_monitor()
        self.page_job.set_task_status("空闲", -1)
        self._enqueue_log("status", "[EXEC_TRACK] cancelled task returned to idle")

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
        self.state.sle_rx_state = LinkState.DISCONNECTED
        self.state.screen_state = LinkState.DISCONNECTED
        self.state.mode_transition = "UNKNOWN"
        self._mode_status_sync_pending = False
        self._mode_status_sync_timer.stop()
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
        self._log_path = session_log_dir() / f"session_{stamp}.log"
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
        worker.status.connect(
            lambda message, current=worker: self._on_ai_image_status(
                current,
                message,
            )
        )
        worker.image_ready.connect(
            lambda path, current=worker: self._on_ai_image_ready(current, path)
        )
        worker.error.connect(
            lambda message, current=worker: self._on_ai_image_error(
                current,
                message,
            )
        )
        worker.cancelled.connect(
            lambda current=worker: self._on_ai_image_cancelled(current)
        )
        worker.finished.connect(
            lambda current=worker: self._on_ai_image_finished(current)
        )
        self._doubao_worker = worker
        worker.start()

    def _on_ai_generation_cancel_requested(self) -> None:
        worker = self._doubao_worker
        if worker is None or not worker.isRunning():
            self.page_gcode.update_ui_generating_state(False)
            self.page_gcode.image_status.setText("当前没有正在进行的豆包生图任务")
            return
        self._enqueue_log("status", "用户请求取消豆包生图")
        worker.cancel()

    def _on_ai_image_status(
        self,
        worker: DoubaoImageWorker,
        message: str,
    ) -> None:
        if worker is self._doubao_worker:
            self.page_gcode.image_status.setText(message)

    def _on_ai_image_ready(self, worker: DoubaoImageWorker, path: str) -> None:
        if worker is not self._doubao_worker:
            self._enqueue_log("status", f"忽略已放弃生图任务的晚到结果: {path}")
            return
        image = QImage(path)
        if image.isNull():
            self._on_ai_image_error(worker, f"生成图片已保存但无法预览: {path}")
            return
        self.page_gcode.set_image(path, image)
        self.page_gcode.image_status.setText(f"豆包生图完成，已保存: {path}")

    def _on_ai_image_error(
        self,
        worker: DoubaoImageWorker,
        message: str,
    ) -> None:
        if worker is not self._doubao_worker:
            return
        self.page_gcode.image_status.setText(message)
        self._enqueue_log("error", f"豆包生图失败: {message}")

    def _on_ai_image_cancelled(self, worker: DoubaoImageWorker) -> None:
        if worker is not self._doubao_worker:
            return
        self.page_gcode.update_ui_generating_state(False)
        self.page_gcode.image_status.setText(
            "已取消豆包生图，可重新生成或导入图片"
        )
        self._enqueue_log("status", "豆包生图已取消")

    def _on_ai_image_finished(self, worker: DoubaoImageWorker) -> None:
        if worker is not self._doubao_worker:
            worker.deleteLater()
            return
        self.page_gcode.update_ui_generating_state(False)
        self._doubao_worker = None
        worker.deleteLater()

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
        if len(gcode) > JOB_MAX_SIZE:
            message = (
                f"仅上传任务不能超过 RX cache: {len(gcode)}/{JOB_MAX_SIZE} bytes；"
                "大文件请使用上传并执行，并保留安全 preroll 切换余量。"
            )
            self._enqueue_log("error", message)
            QMessageBox.warning(self, "任务错误", message)
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
        try:
            self.client.upload_job(
                job_id, gcode, timeout,
                progress_cb=lambda t: self.worker.progress.emit(t),
                status_cb=lambda t, p: self.worker.task_status.emit(t, p),
                preroll_bytes=0,
                start_on_preroll=False,
            )
        except UploadInterrupted as exc:
            self.worker.log.emit("status", f"[CTRL_FAST] {exc}")
            if "@ABORT" in str(exc):
                self.state.termination_requested = True
                self.state.execution_expected = False
                self.state.execution_complete = False
                self._stop_execution_status_poll()
            self.worker.task_status.emit(
                "已取消并清空" if "@ABORT" in str(exc) else "控制命令已接管上传", -1
            )
            return
        self.worker.task_status.emit("上传完成，RX 任务已就绪", 100)

    def _upload_exec_worker(
        self, gcode: bytes, job_id: int, timeout: float, preroll: int
    ) -> None:
        self._focus_off_before_job()
        crc = crc16_ccitt(gcode)
        total = len(gcode)
        requested_preroll = preroll
        plan_requested = requested_preroll
        plan_reason = "disabled"
        effective_preroll = 0
        if requested_preroll > 0:
            plan_requested, capped = clamp_stream_preroll_request(total, requested_preroll)
            if capped:
                self.worker.log.emit(
                    "status",
                    "[EXEC_FLOW] preroll capped "
                    f"requested={requested_preroll} capped={plan_requested} "
                    f"cache={JOB_MAX_SIZE} headroom={JOB_EXEC_PREROLL_CACHE_HEADROOM_BYTES}",
                )
        if plan_requested > 0 and total > plan_requested:
            plan = plan_safe_preroll(gcode, plan_requested)
            effective_preroll = plan.effective
            plan_reason = plan.reason
            self.worker.log.emit(
                "status",
                "[EXEC_FLOW] preroll plan "
                f"requested={plan.requested} effective={plan.effective} "
                f"safe_line={plan.safe_line_end} reason={plan.reason}",
            )
        self.worker.log.emit("status", f"[EXEC_FLOW] 开始上传并执行 job={job_id} size={total} crc=0x{crc:04x} preroll={effective_preroll}")

        # Upload+execute is controlled by the execution preroll setting.
        use_preroll = effective_preroll > 0 and total > effective_preroll
        if total > JOB_MAX_SIZE and not use_preroll:
            if requested_preroll <= 0:
                detail = "未启用 preroll"
            else:
                detail = f"未找到安全 preroll 边界 reason={plan_reason}"
            raise RuntimeError(
                f"大文件 {total} bytes 超过 RX cache {JOB_MAX_SIZE} bytes，"
                f"上传并执行必须先在 cache 内切换到执行；{detail}"
            )
        if requested_preroll > 0 and not use_preroll:
            self.worker.log.emit("status", f"[EXEC_FLOW] 未找到可用 preroll 边界，使用 normal upload 路径")
        self.worker.log.emit("status", f"[EXEC_FLOW] use_preroll={use_preroll} path={'preroll' if use_preroll else 'normal'}")

        # Reset execution state. Completion is received asynchronously from RX.
        self.state.execution_complete = False
        self.state.execution_expected = False
        self.state.termination_requested = False

        try:
            # Upload phase
            self.worker.log.emit("status", f"[EXEC_FLOW] 开始上传阶段")
            self.worker.task_status.emit("上传中...", 0)
            try:
                self.client.upload_job(
                    job_id, gcode, timeout,
                    progress_cb=lambda t: self.worker.progress.emit(t),
                    status_cb=lambda t, p: self.worker.task_status.emit(t, p),
                    preroll_bytes=effective_preroll if use_preroll else 0,
                    start_on_preroll=use_preroll,
                    enforce_job_size_limit=False,
                )
            except UploadInterrupted as exc:
                self.worker.log.emit("status", f"[CTRL_FAST] {exc}")
                if "@ABORT" in str(exc):
                    self.state.termination_requested = True
                    self.state.execution_expected = False
                    self.state.execution_complete = False
                    self._stop_execution_status_poll()
                self.worker.task_status.emit(
                    "已取消并清空" if "@ABORT" in str(exc) else "控制命令已接管上传", -1
                )
                return
            self.worker.log.emit("status", f"[EXEC_FLOW] 上传阶段完成")

            # Execution phase
            if use_preroll:
                # RX owns the threshold transition; DATA upload does not pause.
                self.worker.log.emit(
                    "status", "[EXEC_FLOW] RX 预缓冲自动启动路径，上传期间保持连续 DATA"
                )
                self.worker.task_status.emit("RX 已自动启动，等待执行完成", -1)
            else:
                # Small file: send EXEC_START after upload complete
                self.worker.log.emit("status", f"[EXEC_FLOW] 小文件 normal 路径，发送 EXEC_START")
                self.worker.task_status.emit("执行中...", -1)
                exec_timeout = max(timeout, EXEC_START_ACK_TIMEOUT_MIN_S)
                self.client.send_control(f"@EXEC_START {job_id}", "@ACK type=16", exec_timeout)
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
        self._cancelled_task_timer.stop()
        self._cancelled_job_id = 0
        self.state.prev_rx_state_code = self.state.rx_state_code
        self.state.rx_state_code = 1
        self.state.rx_job_id = job_id
        self.state.rx_received = 0
        self.state.rx_job_total = len(gcode)
        self.state.rx_executed_lines = 0
        self.state.rx_completed_lines = 0
        self.state.rx_total_lines = 0
        self.state.execution_expected = False
        self.state.termination_requested = False
        self.state.execution_complete = False
        self.page_job.set_execution_state(False)
        self._update_monitor()
        self._enqueue_log(
            "status", f"任务上传基线: total_bytes={len(gcode)}"
        )

    def _on_exec_start(self) -> None:
        self._cancelled_task_timer.stop()
        self._cancelled_job_id = 0
        job_id = self.page_job.get_job_id()
        timeout = self.page_job.get_timeout()
        self.state.execution_complete = False
        self.state.termination_requested = False

        def _work():
            exec_timeout = max(timeout, EXEC_START_ACK_TIMEOUT_MIN_S)
            self.client.send_control(f"@EXEC_START {job_id}", "@ACK type=16", exec_timeout)
            self.worker.task_status.emit("执行中，等待 RX 完成", -1)
        self._run_job_worker(_work)

    def _on_exec_stop(self) -> None:
        if self.worker.is_busy():
            queued = self.client.request_priority_control(
                "@EXEC_STOP", "@ACK type=19", self.page_job.get_timeout(),
                interrupt_upload=False,
            )
            if queued:
                self.state.termination_requested = True
                self.state.execution_complete = False
            self._enqueue_log(
                "status",
                f"[CTRL_FAST] 暂停命令已排队，将在当前 {JOB_DATA_CHUNK_SIZE}B 数据包 ACK 后作为下一个 SLE 包发送"
                if queued else "[CTRL_FAST] 已有控制命令等待发送",
            )
            if queued:
                self.page_job.set_task_status("暂停命令等待发送", -1)
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
            self.worker.task_status.emit("已暂停", -1)
        self._run_job_worker(_work)

    def _on_exec_resume(self) -> None:
        if self.worker.is_busy():
            queued = self.client.request_priority_control(
                "@EXEC_RESUME", "@ACK type=17", self.page_job.get_timeout(),
                interrupt_upload=False,
            )
            self._enqueue_log(
                "status",
                f"[CTRL_FAST] 继续命令已排队，将在当前 {JOB_DATA_CHUNK_SIZE}B 数据包 ACK 后作为下一个 SLE 包发送"
                if queued else "[CTRL_FAST] 已有控制命令等待发送",
            )
            if queued:
                self.state.termination_requested = False
                self.state.execution_expected = True
                self.state.execution_complete = False
                self.page_job.set_execution_state(True)
                self._start_execution_status_poll()
            self.page_job.set_task_status("继续命令等待发送", -1)
            return
        timeout = self.page_job.get_timeout()

        def _work():
            try:
                self.client.send_control("@EXEC_RESUME", "@ACK type=17", timeout)
            except Exception:
                self.worker.task_status.emit("继续命令失败", -1)
                raise
            self.state.termination_requested = False
            self.state.execution_complete = False
            self.worker.task_status.emit("执行中，等待 RX 完成", -1)
        self._run_job_worker(_work)

    def _on_abort(self) -> None:
        if self.worker.is_busy():
            queued = self.client.request_priority_control(
                "@ABORT", "@ACK type=4", self.page_job.get_timeout()
            )
            if queued:
                self.state.termination_requested = True
                self.state.execution_complete = False
            self._enqueue_log(
                "status",
                f"[CTRL_FAST] 取消命令已排队，将在当前 {JOB_DATA_CHUNK_SIZE}B 数据包 ACK 后作为下一个 SLE 包发送"
                if queued else "[CTRL_FAST] 已有控制命令等待发送",
            )
            if queued:
                self.page_job.set_task_status("取消命令等待发送", -1)
            return
        timeout = self.page_job.get_timeout()
        self.state.termination_requested = True
        self.state.execution_complete = False

        def _work():
            try:
                self.client.send_control("@ABORT", "@ACK type=4", timeout)
            except Exception:
                self.worker.task_status.emit("取消命令失败", -1)
                raise
            self.worker.task_status.emit("已取消并清空", -1)
        self._run_job_worker(_work)

    def _on_focus_on(self, power: int) -> None:
        if power < 1 or power > 100:
            self.worker.log.emit("error", f"FOCUS_ON 功率无效: {power}，允许范围 1-100")
            return

        def _work():
            self.client.send_control(f"@FOCUS_ON S{power}", "@OK focus_on", 5.0)
            self.focus_state_changed.emit(True)
            self.worker.log.emit("status", f"FOCUS_ON S={power}")
        self._run_job_worker(_work)

    def _on_focus_off(self) -> None:
        if self.worker.is_busy():
            queued = self.client.request_priority_control(
                "@FOCUS_OFF", "@OK focus_off", 5.0, interrupt_upload=False
            )
            self._enqueue_log(
                "status",
                f"[CTRL_FAST] 关光命令已排队，将在当前 {JOB_DATA_CHUNK_SIZE}B 数据包 ACK 后作为下一个 SLE 包发送"
                if queued else "[CTRL_FAST] 已有控制命令等待发送",
            )
            return

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

    def _schedule_mode_status_sync(self) -> None:
        self._mode_status_sync_pending = True
        self._try_mode_status_sync()

    def _try_mode_status_sync(self) -> None:
        if not self._mode_status_sync_pending:
            return
        if not self._client_is_open():
            self._mode_status_sync_pending = False
            return
        if self.worker.is_busy():
            if not self._mode_status_sync_timer.isActive():
                self._mode_status_sync_timer.start()
            return
        self._mode_status_sync_pending = False
        self._enqueue_log("status", "SLE 已恢复，自动同步 RX 状态")
        self._on_status()

    def _apply_focus_state(self, active: bool) -> None:
        self.state.focus_active = active
        self.page_job.set_focus_state(active)
        self._update_status_badges()
        self._update_monitor()

    def _on_mode_selected(self, requested: str) -> None:
        target = self._connection_mode(requested)
        if target is None:
            self._enqueue_log("error", f"不支持的控制模式: {requested}")
            self._refresh_mode_ui()
            return
        if not self._client_is_open():
            self._enqueue_log("error", "TX 命令串口未连接，无法切换控制模式")
            self._refresh_mode_ui()
            return
        if self.worker.is_busy():
            self._enqueue_log("error", "任务进行中，无法切换控制模式")
            self._refresh_mode_ui()
            return

        self.state.mode_target = target
        self.state.mode_transition = "REQUESTING"
        self._refresh_mode_ui()
        command_target = "SLE" if target == ConnectionMode.SLE_VIA_TX else "WIFI"

        def _work() -> None:
            try:
                self.client.send_control(
                    f"@MODE {command_target}",
                    f"@OK mode_request target={command_target}",
                    8.0,
                )
            except Exception as exc:
                self.mode_request_failed.emit(command_target, str(exc))
                raise
            self.mode_request_acknowledged.emit(command_target)

        self._run_job_worker(_work)

    def _on_mode_request_acknowledged(self, requested: str) -> None:
        target = self._connection_mode(requested)
        if target is None or target != self.state.mode_target:
            return
        if target == ConnectionMode.SLE_VIA_TX:
            if self.state.mode_transition not in ("ACTIVE", "FAILED"):
                self.state.mode_transition = "CONNECTING"
            self.page_job.set_task_status("SLE 重连请求已接受", -1)
            self._enqueue_log("status", "MODE_REQUEST SLE accepted；等待 RX 连接确认")
        else:
            if self.state.mode_transition not in ("EXTERNAL_VERIFY", "FAILED"):
                self.state.mode_transition = "PENDING"
            self.page_job.set_task_status("Wi-Fi 切换请求已接受，等待外部验证", 100)
            self._enqueue_log(
                "status", "MODE_REQUEST WIFI accepted；请用 LaserGRBL 验证 Wi-Fi 路由"
            )
        self._refresh_mode_ui()

    def _on_mode_request_failed(self, requested: str, reason: str) -> None:
        target = self._connection_mode(requested)
        if target is None or target != self.state.mode_target:
            return
        if (target == ConnectionMode.SLE_VIA_TX and
                self.state.mode_transition == "ACTIVE"):
            return
        if (target == ConnectionMode.WIFI_TCP and
                self.state.mode_transition == "EXTERNAL_VERIFY"):
            return
        self.state.mode_transition = "FAILED"
        self._refresh_mode_ui(reason=reason)

    def _on_switch_wifi(self) -> None:
        """Compatibility entry point for old callers of the one-way action."""
        self._on_mode_selected("WIFI")

    def _on_save_settings(self, config: HostConfig) -> None:
        self.config_store.save(config)
        self.config = config
        self.page_conn.load_config(config)
        self.page_job.load_config(config)
        self._enqueue_log("host", f"设置已保存: {self.config_store.path}")

    def closeEvent(self, event) -> None:
        if self._doubao_worker is not None and self._doubao_worker.isRunning():
            self._enqueue_log("status", "退出上位机前取消豆包生图")
            self._doubao_worker.cancel()
        if self.worker.is_busy():
            self._enqueue_log("error", "任务操作进行中，拒绝退出上位机")
            event.ignore()
            return
        if not self._focus_off_before_disconnect():
            event.ignore()
            return
        self.client.close()
        self._stop_execution_status_poll()
        self._cancelled_task_timer.stop()
        self._mode_status_sync_pending = False
        self._mode_status_sync_timer.stop()
        self.tx_monitor.close()
        self.rx_monitor.close()
        self._close_log_file()
        super().closeEvent(event)
