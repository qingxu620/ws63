from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtWidgets import QComboBox, QFormLayout, QGroupBox, QLabel, QLineEdit, QPushButton, QSpinBox, QVBoxLayout, QWidget

from app.config_store import HostConfig
from ui.pages.connection_page import BAUDRATES, MODES


class SettingsPage(QWidget):
    save_requested = Signal(HostConfig)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(28, 24, 28, 28)
        layout.setSpacing(18)

        title = QLabel("Settings")
        title.setObjectName("pageTitle")
        caption = QLabel("Save default mode, serial role mapping, and WiFi endpoint.")
        caption.setObjectName("pageCaption")
        caption.setWordWrap(True)

        form_group = QGroupBox("Persistent Settings")
        form = QFormLayout(form_group)
        self.default_mode = QComboBox()
        self.default_mode.addItems(MODES)
        self.rx_direct_port = QLineEdit()
        self.tx_cmd_port = QLineEdit()
        self.tx_log_port = QLineEdit()
        self.rx_log_port = QLineEdit()
        self.baudrate = QComboBox()
        self.baudrate.setEditable(True)
        for baudrate in BAUDRATES:
            self.baudrate.addItem(str(baudrate))
        self.wifi_ip = QLineEdit()
        self.wifi_port = QSpinBox()
        self.wifi_port.setRange(1, 65535)
        self.save_button = QPushButton("Save Settings")

        form.addRow("Default mode", self.default_mode)
        form.addRow("USART RX direct port", self.rx_direct_port)
        form.addRow("SLE TX command port", self.tx_cmd_port)
        form.addRow("SLE TX log port", self.tx_log_port)
        form.addRow("SLE RX log port", self.rx_log_port)
        form.addRow("Default baudrate", self.baudrate)
        form.addRow("WiFi IP", self.wifi_ip)
        form.addRow("WiFi port", self.wifi_port)
        form.addRow("", self.save_button)

        layout.addWidget(title)
        layout.addWidget(caption)
        layout.addWidget(form_group)
        layout.addStretch(1)
        self.save_button.clicked.connect(self._emit_save)

    def load_config(self, config: HostConfig) -> None:
        self.default_mode.setCurrentText(config.default_mode)
        self.rx_direct_port.setText(config.serial.rx_direct_port)
        self.tx_cmd_port.setText(config.serial.tx_cmd_port)
        self.tx_log_port.setText(config.serial.tx_log_port)
        self.rx_log_port.setText(config.serial.rx_log_port)
        self.baudrate.setCurrentText(str(config.serial.baudrate))
        self.wifi_ip.setText(config.wifi.ip)
        self.wifi_port.setValue(config.wifi.port)

    def _emit_save(self) -> None:
        config = HostConfig()
        config.default_mode = self.default_mode.currentText()
        config.serial.rx_direct_port = self.rx_direct_port.text().strip()
        config.serial.tx_cmd_port = self.tx_cmd_port.text().strip()
        config.serial.tx_log_port = self.tx_log_port.text().strip()
        config.serial.rx_log_port = self.rx_log_port.text().strip()
        config.serial.baudrate = self._baudrate()
        config.wifi.ip = self.wifi_ip.text().strip() or "192.168.43.1"
        config.wifi.port = self.wifi_port.value()
        self.save_requested.emit(config)

    def _baudrate(self) -> int:
        try:
            return int(self.baudrate.currentText().strip())
        except ValueError:
            return 115200
