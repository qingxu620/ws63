from __future__ import annotations

from datetime import datetime

from PySide6.QtGui import QTextCursor
from PySide6.QtWidgets import (
    QCheckBox, QFrame, QHBoxLayout, QLabel, QPushButton, QTextEdit,
    QVBoxLayout, QWidget, QFileDialog, QLineEdit,
)

SHOW_ALWAYS_TOKENS = (
    "ERROR", "NACK", "@NACK", "@ERR", "TIMEOUT", "ABORT", "REJECT",
    "SAFE_STOP", "EXEC_DONE",
    "[JOB_EXEC] start", "[JOB_EXEC] done", "JOB_READY", "DATA_DONE",
    "EXEC_START ACK", "上传完成", "开始上传", "开始执行", "执行完成",
    "JOB_SLE_WRITE_CFM_TIMEOUT", "JOB_EXEC_WAIT_TIMEOUT",
    "JOB_RX_NACK_SEND", "JOB_DATA_REJECT", "JOB_CACHE_WRITE_REJECT",
)
HIDE_UI_TOKENS = (
    "ACK_PARSE", "[JOB_DATA_IN]", "[JOB_DATA_HDR]", "[JOB_DATA_RESULT]",
    "[JOB_TX_DATA_ACK_PRE]", "[JOB_TX_HOST_ACK]", "[TX_ACK]", "[RX_ACK]",
    "[JOB_EXEC_STREAM_THROTTLE]", "[JOB_EXEC] line=", "@ACK type=2",
    "update ssap send report handle",
)


class LogsPage(QWidget):
    """Multi-channel log viewer with filtering."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._line_count = 0
        self._paused = False
        self._pending: list[str] = []
        self._all_logs: list[tuple[str, str, str, str]] = []
        self._search_keyword = ""
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setSpacing(12)
        layout.setContentsMargins(24, 20, 24, 20)

        subtitle = QLabel("记录和追踪串口物理报文、接收校验回执以及系统的状态输出流。")
        subtitle.setObjectName("pageSubtitle")
        subtitle.setWordWrap(True)
        layout.addWidget(subtitle)

        # Toolbar Card
        toolbar_card = QWidget()
        toolbar_card.setObjectName("logsToolbar")
        toolbar_card.setStyleSheet("""
            QWidget#logsToolbar {
                background: #ffffff;
                border: 1px solid #e2e8f0;
                border-radius: 8px;
            }
        """)
        toolbar = QHBoxLayout(toolbar_card)
        toolbar.setContentsMargins(16, 8, 16, 8)
        toolbar.setSpacing(20)

        self.chk_diag = QCheckBox("显示诊断日志")
        self.chk_diag.setChecked(False)
        self.chk_diag.setCursor(self.cursor())
        toolbar.addWidget(self.chk_diag)

        self.chk_pause = QCheckBox("暂停滚动")
        self.chk_pause.setCursor(self.cursor())
        self.chk_pause.toggled.connect(self._on_pause_toggle)
        toolbar.addWidget(self.chk_pause)
        toolbar.addStretch(1)

        self.txt_search = QLineEdit()
        self.txt_search.setPlaceholderText("🔍 搜索日志...")
        self.txt_search.setClearButtonEnabled(True)
        self.txt_search.setFixedWidth(160)
        self.txt_search.textChanged.connect(self._on_search_changed)
        toolbar.addWidget(self.txt_search)

        self.btn_export = QPushButton("导出日志")
        self.btn_export.setObjectName("btnPrimary")
        self.btn_export.setCursor(self.cursor())
        self.btn_export.clicked.connect(self._export_logs)
        toolbar.addWidget(self.btn_export)

        self.btn_clear = QPushButton("清空日志")
        self.btn_clear.setCursor(self.cursor())
        self.btn_clear.clicked.connect(self._clear)
        toolbar.addWidget(self.btn_clear)
        layout.addWidget(toolbar_card)

        terminal = QFrame()
        terminal.setObjectName("terminalCard")
        terminal.setStyleSheet(
            "QFrame#terminalCard { background:#ffffff; border:1px solid #e2e8f0; border-radius:10px; }"
        )
        terminal_layout = QVBoxLayout(terminal)
        terminal_layout.setContentsMargins(0, 0, 0, 0)
        terminal_layout.setSpacing(0)
        terminal_header = QLabel("实时串口日志控制台")
        terminal_header.setObjectName("terminalHeader")
        terminal_header.setStyleSheet(
            "background:#f8fafc; color:#475569; border:none; border-bottom:1px solid #e2e8f0; "
            "padding:10px 14px; font-size:12px; font-weight:600;"
        )
        terminal_layout.addWidget(terminal_header)

        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setFontFamily("Consolas")
        self.log_text.setFontPointSize(10)
        self.log_text.setStyleSheet("""
            QTextEdit {
                background: #ffffff;
                color: #1e293b;
                border: none;
                border-radius: 0;
                padding: 16px;
            }
        """)
        terminal_layout.addWidget(self.log_text, 1)
        layout.addWidget(terminal, 1)

    def append_log(self, channel: str, message: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        kind_map = {
            "tx_log": "发送",
            "rx_log": "接收",
            "tx": "发送",
            "rx": "接收",
            "status": "状态",
            "error": "错误",
            "host": "主机"
        }
        kind = kind_map.get(channel, channel.upper())

        if not self._should_show(channel, message):
            return

        # Color coding for channels
        color_map = {
            "发送": "#0284C7",
            "接收": "#047857",
            "错误": "#B91C1C",
            "状态": "#B45309",
            "主机": "#2563EB"
        }
        chan_color = color_map.get(kind, "#2563EB")
        
        # Padded version of channel for neat formatting
        kind_padded = kind.ljust(4) if len(kind) < 4 else kind
        
        # Build HTML line
        clean_msg = message.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        html_line = (
            f'<span style="color: #64748B;">[{stamp}]</span> '
            f'<span style="color: {chan_color}; font-weight: bold;">{kind_padded}</span> '
            f'<span style="color: #1E293B;">{clean_msg}</span>'
        )

        # Store in log cache (up to 4000 entries)
        self._all_logs.append((stamp, channel, message, html_line))
        if len(self._all_logs) > 4000:
            self._all_logs = self._all_logs[-3500:]

        if self._paused:
            self._pending.append(html_line)
            if len(self._pending) > 2000:
                self._pending = self._pending[-1000:]
            return

        # Apply search filter
        if self._search_keyword and self._search_keyword not in message.lower():
            return

        self._append(html_line)

    def _append(self, line: str) -> None:
        # Check if scrollbar is currently at the bottom (with a small 20px threshold)
        bar = self.log_text.verticalScrollBar()
        was_at_bottom = (bar.value() >= bar.maximum() - 20) or (bar.maximum() == 0)

        self.log_text.append(line)
        self._line_count += 1
        if self._line_count > 4000:
            cursor = self.log_text.textCursor()
            cursor.movePosition(QTextCursor.MoveOperation.Start)
            cursor.movePosition(
                QTextCursor.MoveOperation.Down,
                QTextCursor.MoveMode.KeepAnchor,
                500,
            )
            cursor.removeSelectedText()
            self._line_count -= 500

        # Auto-scroll only if we are not paused and user was already at the bottom
        if not self._paused and was_at_bottom:
            bar.setValue(bar.maximum())

    def _should_show(self, channel: str, message: str) -> bool:
        if self.chk_diag.isChecked():
            return True
        if channel == "error":
            return True
        for t in SHOW_ALWAYS_TOKENS:
            if t in message:
                return True
        for t in HIDE_UI_TOKENS:
            if t in message:
                return False
        return True

    def _on_pause_toggle(self, paused: bool) -> None:
        self._paused = paused
        if not paused and self._pending:
            for line in self._pending:
                self._append(line)
            self._pending.clear()

    def _clear(self) -> None:
        self.log_text.clear()
        self._line_count = 0
        self._all_logs.clear()

    def _on_search_changed(self, text: str) -> None:
        self._search_keyword = text.strip().lower()
        self._refilter_logs()

    def _refilter_logs(self) -> None:
        self.log_text.clear()
        self._line_count = 0

        lines_to_add = []
        for stamp, channel, message, html_line in self._all_logs:
            if not self._search_keyword or self._search_keyword in message.lower():
                lines_to_add.append(html_line)

        self.log_text.setUpdatesEnabled(False)
        for line in lines_to_add:
            self._append(line)
        self.log_text.setUpdatesEnabled(True)
        self.log_text.update()

        # Re-scroll to bottom
        bar = self.log_text.verticalScrollBar()
        bar.setValue(bar.maximum())

    def _export_logs(self) -> None:
        filename, _ = QFileDialog.getSaveFileName(
            self, "导出日志", "ws63_logs.txt", "文本文件 (*.txt)"
        )
        if filename:
            try:
                with open(filename, "w", encoding="utf-8") as f:
                    f.write(self.log_text.toPlainText())
            except Exception as e:
                self.append_log("error", f"导出日志失败: {e}")
