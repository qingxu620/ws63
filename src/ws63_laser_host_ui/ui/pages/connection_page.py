from __future__ import annotations

from dataclasses import dataclass

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QComboBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QSpinBox,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from app.config_store import HostConfig
from ui.widgets.port_selector import PortSelector
from utils.serial_ports import SerialPortInfo


MODE_USART = "USART Direct"
MODE_SLE = "SLE via TX"
MODE_WIFI = "WiFi TCP"
MODES = (MODE_USART, MODE_SLE, MODE_WIFI)
BAUDRATES = (115200, 230400, 460800, 921600)


@dataclass(slots=True)
class UsartDirectForm:
    rx_direct_port: str
    baudrate: int


@dataclass(slots=True)
class SleViaTxForm:
    tx_cmd_port: str
    tx_log_port: str
    rx_log_port: str
    baudrate: int


@dataclass(slots=True)
class WifiTcpForm:
    ip: str
    port: int


class ConnectionPage(QWidget):
    refresh_ports_requested = Signal()
    mode_changed = Signal(str)
    usart_connect_requested = Signal()
    sle_connect_requested = Signal()
    wifi_connect_requested = Signal()
    disconnect_requested = Signal()

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(28, 24, 28, 28)
        layout.setSpacing(18)

        title = QLabel("Connection")
        title.setObjectName("pageTitle")
        caption = QLabel("Select one transport mode and connect the matching device path.")
        caption.setObjectName("pageCaption")
        caption.setWordWrap(True)
        layout.addWidget(title)
        layout.addWidget(caption)

        mode_group = QGroupBox("Mode")
        mode_layout = QHBoxLayout(mode_group)
        self.mode_combo = QComboBox()
        self.mode_combo.addItems(MODES)
        self.refresh_ports_button = QPushButton("Refresh Ports")
        mode_layout.addWidget(QLabel("Connection mode"))
        mode_layout.addWidget(self.mode_combo, 1)
        mode_layout.addWidget(self.refresh_ports_button)
        layout.addWidget(mode_group)

        self.stack = QStackedWidget()
        self.stack.addWidget(self._build_usart_panel())
        self.stack.addWidget(self._build_sle_panel())
        self.stack.addWidget(self._build_wifi_panel())
        layout.addWidget(self.stack)
        layout.addStretch(1)

        self.mode_combo.currentIndexChanged.connect(self._on_mode_index_changed)
        self.refresh_ports_button.clicked.connect(self.refresh_ports_requested.emit)

    def _build_usart_panel(self) -> QWidget:
        group = QGroupBox("USART Direct")
        layout = QFormLayout(group)
        self.rx_direct_port = PortSelector()
        self.usart_baud = self._baud_combo()
        self.usart_connect_button = QPushButton("Connect")
        self.usart_disconnect_button = QPushButton("Disconnect")
        self.usart_connect_button.clicked.connect(self.usart_connect_requested.emit)
        self.usart_disconnect_button.clicked.connect(self.disconnect_requested.emit)
        layout.addRow("RX port", self.rx_direct_port)
        layout.addRow("Baudrate", self.usart_baud)
        layout.addRow("", self._button_row(self.usart_connect_button, self.usart_disconnect_button))
        return group

    def _build_sle_panel(self) -> QWidget:
        group = QGroupBox("SLE via TX")
        layout = QFormLayout(group)
        self.tx_cmd_port = PortSelector()
        self.tx_log_port = PortSelector()
        self.rx_log_port = PortSelector()
        self.sle_baud = self._baud_combo()
        self.sle_connect_button = QPushButton("Connect")
        self.sle_disconnect_button = QPushButton("Disconnect")
        self.sle_connect_button.clicked.connect(self.sle_connect_requested.emit)
        self.sle_disconnect_button.clicked.connect(self.disconnect_requested.emit)
        layout.addRow("TX command port", self.tx_cmd_port)
        layout.addRow("TX log port", self.tx_log_port)
        layout.addRow("RX log port", self.rx_log_port)
        layout.addRow("Baudrate", self.sle_baud)
        layout.addRow("", self._button_row(self.sle_connect_button, self.sle_disconnect_button))
        return group

    def _build_wifi_panel(self) -> QWidget:
        group = QGroupBox("WiFi TCP")
        layout = QFormLayout(group)
        self.wifi_ip = QLineEdit()
        self.wifi_port = QSpinBox()
        self.wifi_port.setRange(1, 65535)
        self.wifi_connect_button = QPushButton("Connect")
        self.wifi_disconnect_button = QPushButton("Disconnect")
        self.wifi_connect_button.clicked.connect(self.wifi_connect_requested.emit)
        self.wifi_disconnect_button.clicked.connect(self.disconnect_requested.emit)
        layout.addRow("IP address", self.wifi_ip)
        layout.addRow("TCP port", self.wifi_port)
        layout.addRow("", self._button_row(self.wifi_connect_button, self.wifi_disconnect_button))
        return group

    def _button_row(self, *buttons: QPushButton) -> QWidget:
        row = QWidget()
        layout = QHBoxLayout(row)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addStretch(1)
        for button in buttons:
            layout.addWidget(button)
        return row

    def _baud_combo(self) -> QComboBox:
        combo = QComboBox()
        combo.setEditable(True)
        for baudrate in BAUDRATES:
            combo.addItem(str(baudrate))
        combo.setCurrentText("115200")
        return combo

    def _on_mode_index_changed(self, index: int) -> None:
        self.stack.setCurrentIndex(index)
        self.mode_changed.emit(self.current_mode())

    def set_ports(self, ports: list[SerialPortInfo]) -> None:
        self.rx_direct_port.set_ports(ports)
        self.tx_cmd_port.set_ports(ports)
        self.tx_log_port.set_ports(ports)
        self.rx_log_port.set_ports(ports)

    def load_config(self, config: HostConfig) -> None:
        self.mode_combo.setCurrentText(config.default_mode)
        self.rx_direct_port.set_device(config.serial.rx_direct_port)
        self.tx_cmd_port.set_device(config.serial.tx_cmd_port)
        self.tx_log_port.set_device(config.serial.tx_log_port)
        self.rx_log_port.set_device(config.serial.rx_log_port)
        self.usart_baud.setCurrentText(str(config.serial.baudrate))
        self.sle_baud.setCurrentText(str(config.serial.baudrate))
        self.wifi_ip.setText(config.wifi.ip)
        self.wifi_port.setValue(config.wifi.port)

    def current_mode(self) -> str:
        return self.mode_combo.currentText()

    def usart_form(self) -> UsartDirectForm:
        return UsartDirectForm(
            rx_direct_port=self.rx_direct_port.device(),
            baudrate=self._combo_int(self.usart_baud, 115200),
        )

    def sle_form(self) -> SleViaTxForm:
        return SleViaTxForm(
            tx_cmd_port=self.tx_cmd_port.device(),
            tx_log_port=self.tx_log_port.device(),
            rx_log_port=self.rx_log_port.device(),
            baudrate=self._combo_int(self.sle_baud, 115200),
        )

    def wifi_form(self) -> WifiTcpForm:
        return WifiTcpForm(
            ip=self.wifi_ip.text().strip(),
            port=self.wifi_port.value(),
        )

    def _combo_int(self, combo: QComboBox, default: int) -> int:
        try:
            return int(combo.currentText().strip())
        except ValueError:
            return default
