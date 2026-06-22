from __future__ import annotations

from dataclasses import replace

from PySide6.QtCore import Signal
from PySide6.QtGui import QDoubleValidator, QIntValidator
from PySide6.QtWidgets import (
    QComboBox,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from app.config_store import HostConfig


class ConnectionPage(QWidget):
    """Serial connection controls and persisted system defaults."""

    connect_requested = Signal(str, str, int)
    disconnect_requested = Signal()
    tx_log_requested = Signal(str, int)
    rx_log_requested = Signal(str, int)
    stop_tx_log_requested = Signal()
    stop_rx_log_requested = Signal()
    refresh_requested = Signal()
    save_requested = Signal(HostConfig)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._config = HostConfig()
        self._build_ui()

    @staticmethod
    def _field_row(label: str, field: QWidget, label_width: int = 145) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(12)
        caption = QLabel(label)
        caption.setFixedWidth(label_width)
        row.addWidget(caption)
        row.addWidget(field, 1)
        return row

    @staticmethod
    def _card_layout(group: QGroupBox) -> QVBoxLayout:
        layout = QVBoxLayout(group)
        layout.setSpacing(10)
        layout.setContentsMargins(16, 22, 16, 14)
        return layout

    @staticmethod
    def _compact_combo(combo: QComboBox) -> None:
        combo.setMinimumWidth(0)
        combo.setMinimumContentsLength(10)
        combo.setSizeAdjustPolicy(
            QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon
        )
        combo.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setSpacing(12)
        layout.setContentsMargins(24, 20, 24, 20)

        subtitle = QLabel("配置串口通道参数及系统的默认运行预设值。")
        subtitle.setObjectName("pageSubtitle")
        layout.addWidget(subtitle)

        cmd_group = QGroupBox("命令发送端口 (TX)")
        cmd_layout = self._card_layout(cmd_group)

        self.cmd_combo = QComboBox()
        self._compact_combo(self.cmd_combo)
        self.btn_refresh = QPushButton("刷新")
        self.btn_refresh.clicked.connect(self.refresh_requested.emit)
        port_row = self._field_row("选择端口", self.cmd_combo, 110)
        port_row.addWidget(self.btn_refresh)
        cmd_layout.addLayout(port_row)

        self.baud_edit = QLineEdit("115200")
        self.baud_edit.setValidator(QIntValidator(9600, 4_000_000, self))
        cmd_layout.addLayout(self._field_row("波特率", self.baud_edit, 110))
        cmd_layout.addStretch(1)

        connect_row = QHBoxLayout()
        connect_row.setSpacing(12)
        connect_row.addSpacing(122)
        self.btn_connect = QPushButton("连接")
        self.btn_connect.setObjectName("btnPrimary")
        self.btn_connect.clicked.connect(self._on_connect)
        connect_row.addWidget(self.btn_connect, 1)
        cmd_layout.addLayout(connect_row)

        log_group = QGroupBox("调试日志监控")
        log_layout = self._card_layout(log_group)

        self.tx_log_combo = QComboBox()
        self._compact_combo(self.tx_log_combo)
        self.btn_tx_log = QPushButton("监听")
        self.btn_tx_log.setObjectName("btnPrimary")
        self.btn_tx_log.clicked.connect(self._on_tx_log)
        tx_row = self._field_row("发送端日志 (TX)", self.tx_log_combo, 130)
        tx_row.addWidget(self.btn_tx_log)
        log_layout.addLayout(tx_row)

        self.rx_log_combo = QComboBox()
        self._compact_combo(self.rx_log_combo)
        self.btn_rx_log = QPushButton("监听")
        self.btn_rx_log.setObjectName("btnPrimary")
        self.btn_rx_log.clicked.connect(self._on_rx_log)
        rx_row = self._field_row("接收端日志 (RX)", self.rx_log_combo, 130)
        rx_row.addWidget(self.btn_rx_log)
        log_layout.addLayout(rx_row)
        log_layout.addStretch(1)

        conn_group = QGroupBox("串口连接默认值")
        conn_layout = self._card_layout(conn_group)
        self.tx_cmd_edit = QLineEdit()
        self.tx_cmd_edit.setPlaceholderText("例如 COM8")
        conn_layout.addLayout(self._field_row("默认命令发送端口 (TX)", self.tx_cmd_edit))
        self.tx_log_edit = QLineEdit()
        self.tx_log_edit.setPlaceholderText("例如 COM24")
        conn_layout.addLayout(self._field_row("默认发送端日志端口", self.tx_log_edit))
        self.rx_log_edit = QLineEdit()
        self.rx_log_edit.setPlaceholderText("例如 COM26")
        conn_layout.addLayout(self._field_row("默认接收端日志端口", self.rx_log_edit))
        self.baud_edit_cfg = QLineEdit("115200")
        self.baud_edit_cfg.setValidator(QIntValidator(9600, 4_000_000, self))
        conn_layout.addLayout(self._field_row("物理波特率", self.baud_edit_cfg))

        job_group = QGroupBox("任务控制默认值")
        job_layout = self._card_layout(job_group)
        self.timeout_edit = QLineEdit("20")
        self.timeout_edit.setValidator(QDoubleValidator(1.0, 3600.0, 2, self))
        job_layout.addLayout(self._field_row("指令响应超时 (秒)", self.timeout_edit))
        self.preroll_edit = QLineEdit("4096")
        self.preroll_edit.setValidator(QIntValidator(0, 100_000_000, self))
        job_layout.addLayout(self._field_row("预读缓冲区大小 (字节)", self.preroll_edit))
        self.job_id_edit = QLineEdit("1")
        self.job_id_edit.setValidator(QIntValidator(1, 2_147_483_647, self))
        job_layout.addLayout(self._field_row("默认上传任务 ID", self.job_id_edit))
        self.focus_power_edit = QLineEdit("10")
        self.focus_power_edit.setValidator(QIntValidator(0, 100, self))
        job_layout.addLayout(self._field_row("红光聚焦预设功率", self.focus_power_edit))

        self.cards_grid = QGridLayout()
        self.cards_grid.setHorizontalSpacing(20)
        self.cards_grid.setVerticalSpacing(12)
        self.cards_grid.addWidget(cmd_group, 0, 0)
        self.cards_grid.addWidget(log_group, 0, 1)
        self.cards_grid.addWidget(conn_group, 1, 0)
        self.cards_grid.addWidget(job_group, 1, 1)
        self.cards_grid.setColumnStretch(0, 1)
        self.cards_grid.setColumnStretch(1, 1)
        layout.addLayout(self.cards_grid)

        save_row = QHBoxLayout()
        self.btn_save = QPushButton("保存系统设置")
        self.btn_save.setObjectName("btnPrimary")
        self.btn_save.setMinimumHeight(40)
        self.btn_save.clicked.connect(self._on_save)
        save_row.addWidget(self.btn_save)
        self.lbl_status = QLabel("")
        self.lbl_status.setObjectName("saveStatus")
        save_row.addWidget(self.lbl_status)
        save_row.addStretch(1)
        layout.addLayout(save_row)
        layout.addStretch(1)

    def set_ports(self, ports: list[str]) -> None:
        for combo in (self.cmd_combo, self.tx_log_combo, self.rx_log_combo):
            current = combo.currentText()
            combo.clear()
            combo.addItems(ports)
            for i, port in enumerate(ports):
                if port == current:
                    combo.setCurrentIndex(i)
                    break

    def load_config(self, config: HostConfig) -> None:
        self._config = config
        self.baud_edit.setText(str(config.baudrate))
        self._select_by_prefix(self.cmd_combo, config.tx_cmd_port)
        self._select_by_prefix(self.tx_log_combo, config.tx_log_port)
        self._select_by_prefix(self.rx_log_combo, config.rx_log_port)
        self.tx_cmd_edit.setText(config.tx_cmd_port)
        self.tx_log_edit.setText(config.tx_log_port)
        self.rx_log_edit.setText(config.rx_log_port)
        self.baud_edit_cfg.setText(str(config.baudrate))
        self.timeout_edit.setText(str(config.timeout_cmd))
        self.preroll_edit.setText(str(config.preroll_bytes))
        self.job_id_edit.setText(str(config.job_id))
        self.focus_power_edit.setText(str(config.focus_power))

    def get_baud(self) -> int:
        try:
            return max(9600, int(self.baud_edit.text().strip()))
        except ValueError:
            return 115200

    def set_connected(self, connected: bool) -> None:
        self.btn_connect.setText("断开连接" if connected else "连接")
        self._repolish(self.btn_connect, "btnDanger" if connected else "btnPrimary")
        self.cmd_combo.setEnabled(not connected)
        self.baud_edit.setEnabled(not connected)

    @staticmethod
    def _repolish(button: QPushButton, object_name: str) -> None:
        button.setObjectName(object_name)
        button.style().unpolish(button)
        button.style().polish(button)

    def set_tx_log_active(self, active: bool) -> None:
        self.btn_tx_log.setText("停止监听" if active else "监听")
        self._repolish(self.btn_tx_log, "btnDanger" if active else "btnPrimary")
        self.tx_log_combo.setEnabled(not active)

    def set_rx_log_active(self, active: bool) -> None:
        self.btn_rx_log.setText("停止监听" if active else "监听")
        self._repolish(self.btn_rx_log, "btnDanger" if active else "btnPrimary")
        self.rx_log_combo.setEnabled(not active)

    def _on_connect(self) -> None:
        if self.btn_connect.text() == "断开连接":
            self.disconnect_requested.emit()
            return
        display = self.cmd_combo.currentText().strip()
        if display:
            from transports.sle_tx_transport import port_device

            self.connect_requested.emit(port_device(display), display, self.get_baud())

    def _on_tx_log(self) -> None:
        if self.btn_tx_log.text() == "停止监听":
            self.stop_tx_log_requested.emit()
            return
        display = self.tx_log_combo.currentText().strip()
        if display:
            from transports.sle_tx_transport import port_device

            self.tx_log_requested.emit(port_device(display), self.get_baud())

    def _on_rx_log(self) -> None:
        if self.btn_rx_log.text() == "停止监听":
            self.stop_rx_log_requested.emit()
            return
        display = self.rx_log_combo.currentText().strip()
        if display:
            from transports.sle_tx_transport import port_device

            self.rx_log_requested.emit(port_device(display), self.get_baud())

    def _on_save(self) -> None:
        try:
            config = replace(
                self._config,
                tx_cmd_port=self.tx_cmd_edit.text().strip(),
                tx_log_port=self.tx_log_edit.text().strip(),
                rx_log_port=self.rx_log_edit.text().strip(),
                baudrate=int(self.baud_edit_cfg.text().strip()),
                timeout_cmd=float(self.timeout_edit.text().strip()),
                preroll_bytes=int(self.preroll_edit.text().strip()),
                job_id=int(self.job_id_edit.text().strip()),
                focus_power=int(self.focus_power_edit.text().strip()),
            )
            self.save_requested.emit(config)
            self.lbl_status.setText("系统设置保存完毕！")
            self.lbl_status.setStyleSheet("color: #10b981; font-weight: 600;")
        except Exception as exc:
            self.lbl_status.setText(f"保存出错: {exc}")
            self.lbl_status.setStyleSheet("color: #ef4444; font-weight: 600;")

    @staticmethod
    def _select_by_prefix(combo: QComboBox, prefix: str) -> None:
        if not prefix:
            return
        for i in range(combo.count()):
            if combo.itemText(i).upper().startswith(prefix.upper()):
                combo.setCurrentIndex(i)
                return
