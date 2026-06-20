from __future__ import annotations

from PySide6.QtCore import QThread, Qt
from PySide6.QtGui import QAction
from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QSizePolicy,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from app.app_state import AppState, ConnectionMode, LaserState, LinkState, SleState
from app.config_store import ConfigStore, HostConfig
from app.event_bus import EventBus
from transports.serial_transport import SerialTransport, SerialTransportConfig
from transports.sle_tx_transport import SleTxConfig, SleTxTransport
from workers.serial_worker import SerialWorker, SerialWorkerHandle
from ui.pages.connection_page import MODE_SLE, MODE_USART, MODE_WIFI, ConnectionPage
from ui.pages.logs_page import LogsPage
from ui.pages.settings_page import SettingsPage
from ui.widgets.status_badge import StatusBadge
from utils.serial_ports import list_serial_ports


class MainWindow(QMainWindow):
    def __init__(self, config_store: ConfigStore, event_bus: EventBus) -> None:
        super().__init__()
        self.config_store = config_store
        self.event_bus = event_bus
        self.config = self.config_store.load()
        self.state = AppState()
        self.active_transport = None
        self.serial_workers: dict[str, SerialWorkerHandle] = {}

        self.setWindowTitle("WS63 Laser Host")
        self._build_actions()
        self._build_ui()
        self._connect_signals()
        self.refresh_ports()
        self._load_config()
        self._update_state_from_mode(self.connection_page.current_mode())
        self.event_bus.emit_log("host", "UI V1 ready")
        self.event_bus.emit_log("status", "RX DISCONNECTED, TX DISCONNECTED, SLE IDLE, LASER OFF")

    def _build_actions(self) -> None:
        refresh_action = QAction("Refresh Ports", self)
        refresh_action.triggered.connect(self.refresh_ports)
        self.addAction(refresh_action)

    def _build_ui(self) -> None:
        root = QWidget()
        root_layout = QVBoxLayout(root)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(0)

        root_layout.addWidget(self._build_status_strip())

        body = QWidget()
        body_layout = QHBoxLayout(body)
        body_layout.setContentsMargins(0, 0, 0, 0)
        body_layout.setSpacing(0)

        self.nav = QListWidget()
        self.nav.setObjectName("nav")
        self.nav.setFixedWidth(210)
        for name in ("Connection", "Import", "G-code Preview", "Job", "Monitor", "Logs", "Settings"):
            item = QListWidgetItem(name)
            item.setTextAlignment(Qt.AlignVCenter)
            self.nav.addItem(item)

        self.connection_page = ConnectionPage()
        self.logs_page = LogsPage()
        self.settings_page = SettingsPage()

        self.pages = QStackedWidget()
        self.pages.addWidget(self.connection_page)
        self.pages.addWidget(self._build_placeholder_page("Import", "Image and file import are reserved for a later phase."))
        self.pages.addWidget(self._build_placeholder_page("G-code Preview", "G-code rendering is not implemented in V1."))
        self.pages.addWidget(self._build_placeholder_page("Job", "Job upload and SLE job packets are not implemented in V1."))
        self.pages.addWidget(self._build_monitor_page())
        self.pages.addWidget(self.logs_page)
        self.pages.addWidget(self.settings_page)

        body_layout.addWidget(self.nav)
        body_layout.addWidget(self.pages, 1)
        root_layout.addWidget(body, 1)

        self.setCentralWidget(root)
        self.nav.setCurrentRow(0)
        self._apply_style()

    def _build_status_strip(self) -> QWidget:
        strip = QFrame()
        strip.setObjectName("statusStrip")
        layout = QHBoxLayout(strip)
        layout.setContentsMargins(18, 12, 18, 12)
        layout.setSpacing(14)

        title = QLabel("WS63 Laser Host")
        title.setObjectName("appTitle")
        layout.addWidget(title)
        layout.addStretch(1)

        self.mode_badge = StatusBadge("MODE", "SLE")
        self.rx_badge = StatusBadge("RX", "DISCONNECTED")
        self.tx_badge = StatusBadge("TX", "DISCONNECTED")
        self.sle_badge = StatusBadge("SLE", "IDLE")
        self.laser_badge = StatusBadge("LASER", "OFF")
        for badge in (self.mode_badge, self.rx_badge, self.tx_badge, self.sle_badge, self.laser_badge):
            layout.addWidget(badge)
        return strip

    def _connect_signals(self) -> None:
        self.nav.currentRowChanged.connect(self.pages.setCurrentIndex)
        self.connection_page.refresh_ports_requested.connect(self.refresh_ports)
        self.connection_page.mode_changed.connect(self._on_mode_changed)
        self.connection_page.usart_connect_requested.connect(self._connect_usart)
        self.connection_page.sle_connect_requested.connect(self._connect_sle)
        self.connection_page.wifi_connect_requested.connect(self._connect_wifi_placeholder)
        self.connection_page.disconnect_requested.connect(self._disconnect_active)
        self.settings_page.save_requested.connect(self._save_settings)
        self.event_bus.log_message.connect(self.logs_page.append_log)
        self.event_bus.status_message.connect(self._show_status_message)

    def _load_config(self) -> None:
        self.connection_page.load_config(self.config)
        self.settings_page.load_config(self.config)

    def refresh_ports(self) -> None:
        ports = list_serial_ports()
        self.connection_page.set_ports(ports)
        names = ", ".join(port.display_name for port in ports) if ports else "(none)"
        self.event_bus.emit_log("host", f"Serial ports refreshed: {names}")

    def _on_mode_changed(self, mode: str) -> None:
        self._update_state_from_mode(mode)
        self.event_bus.emit_status(f"MODE {mode}")

    def _update_state_from_mode(self, mode: str) -> None:
        if mode == MODE_USART:
            self.state.mode = ConnectionMode.USART_DIRECT
        elif mode == MODE_WIFI:
            self.state.mode = ConnectionMode.WIFI_TCP
        else:
            self.state.mode = ConnectionMode.SLE_VIA_TX
        self._render_state()

    def _connect_usart(self) -> None:
        self._disconnect_active(log_if_idle=False)
        form = self.connection_page.usart_form()
        if not form.rx_direct_port:
            self.event_bus.emit_log("error", "USART Direct: RX port is required")
            return

        self._start_serial_worker("rx_log", form.rx_direct_port, form.baudrate)
        self.state.rx_state = LinkState.CONNECTING
        self.state.tx_state = LinkState.DISCONNECTED
        self.state.sle_state = SleState.IDLE
        self.state.laser_state = LaserState.OFF
        self._render_state()
        self.event_bus.emit_log("host", f"USART Direct opening {form.rx_direct_port} @ {form.baudrate}")

    def _connect_sle(self) -> None:
        self._disconnect_active(log_if_idle=False)
        form = self.connection_page.sle_form()
        if not form.tx_cmd_port:
            self.event_bus.emit_log("error", "SLE via TX: TX command port is required")
            return

        transport = SleTxTransport(
            SleTxConfig(
                tx_cmd_port=form.tx_cmd_port,
                tx_log_port=form.tx_log_port,
                rx_log_port=form.rx_log_port,
                baudrate=form.baudrate,
            )
        )
        result = transport.connect()
        if not result.ok:
            self.state.tx_state = LinkState.ERROR
            self.state.sle_state = SleState.ERROR
            self._render_state()
            self.event_bus.emit_log("error", result.message)
            return

        self.active_transport = transport
        self.state.tx_state = LinkState.CONNECTED
        self.state.rx_state = LinkState.DISCONNECTED
        self.state.sle_state = SleState.IDLE
        self.state.laser_state = LaserState.OFF
        self._render_state()
        self.event_bus.emit_log("host", result.message)

        if form.tx_log_port:
            self._start_serial_worker("tx_log", form.tx_log_port, form.baudrate)
        if form.rx_log_port:
            self._start_serial_worker("rx_log", form.rx_log_port, form.baudrate)

    def _connect_wifi_placeholder(self) -> None:
        self._disconnect_active(log_if_idle=False)
        form = self.connection_page.wifi_form()
        self.state.mode = ConnectionMode.WIFI_TCP
        self.state.rx_state = LinkState.DISCONNECTED
        self.state.tx_state = LinkState.DISCONNECTED
        self.state.sle_state = SleState.IDLE
        self.state.laser_state = LaserState.OFF
        self._render_state()
        self.event_bus.emit_log("host", f"WiFi TCP UI ready for {form.ip}:{form.port}; real connect is not enabled in V1")

    def _disconnect_active(self, log_if_idle: bool = True) -> None:
        had_work = bool(self.serial_workers) or self.active_transport is not None
        for channel in list(self.serial_workers):
            self._stop_serial_worker(channel)

        if self.active_transport is not None:
            result = self.active_transport.disconnect()
            self.event_bus.emit_log("host" if result.ok else "error", result.message)
            self.active_transport = None

        if had_work or log_if_idle:
            self.event_bus.emit_log("host", "Disconnected")
        self.state.rx_state = LinkState.DISCONNECTED
        self.state.tx_state = LinkState.DISCONNECTED
        self.state.sle_state = SleState.IDLE
        self.state.laser_state = LaserState.OFF
        self._render_state()

    def _start_serial_worker(self, channel: str, port: str, baudrate: int) -> None:
        self._stop_serial_worker(channel)
        thread = QThread(self)
        worker = SerialWorker(channel=channel, port=port, baudrate=baudrate)
        worker.moveToThread(thread)
        worker.line_received.connect(self._on_serial_line)
        worker.status_changed.connect(self._on_serial_status)
        worker.error_occurred.connect(self._on_serial_error)
        worker.finished.connect(thread.quit)
        worker.finished.connect(worker.deleteLater)
        thread.finished.connect(thread.deleteLater)
        thread.started.connect(worker.open)
        self.serial_workers[channel] = SerialWorkerHandle(worker, thread)
        thread.start()

    def _stop_serial_worker(self, channel: str) -> None:
        handle = self.serial_workers.pop(channel, None)
        if handle is None:
            return
        handle.stop()

    def _on_serial_line(self, channel: str, line: str) -> None:
        self.event_bus.emit_log(channel, line)

    def _on_serial_status(self, channel: str, status: str) -> None:
        if channel == "rx_log":
            self.state.rx_state = self._link_state_from_worker(status)
        elif channel == "tx_log":
            self.state.tx_state = self._link_state_from_worker(status)
        self._render_state()
        self.event_bus.emit_log("status", f"{channel} {status}")

    def _on_serial_error(self, channel: str, message: str) -> None:
        self.event_bus.emit_log("error", f"{channel}: {message}")

    def _link_state_from_worker(self, status: str) -> LinkState:
        if status == "CONNECTED":
            return LinkState.CONNECTED
        if status == "CONNECTING":
            return LinkState.CONNECTING
        if status == "ERROR":
            return LinkState.ERROR
        return LinkState.DISCONNECTED

    def _save_settings(self, config: HostConfig) -> None:
        self.config_store.save(config)
        self.config = config
        self.connection_page.load_config(config)
        self.settings_page.load_config(config)
        self.event_bus.emit_status(f"SETTINGS saved {self.config_store.path}")
        QMessageBox.information(self, "Settings", "Settings saved.")

    def _render_state(self) -> None:
        labels = self.state.as_labels()
        self.mode_badge.set_value(labels["mode"])
        self.rx_badge.set_value(labels["rx"])
        self.tx_badge.set_value(labels["tx"])
        self.sle_badge.set_value(labels["sle"])
        self.laser_badge.set_value(labels["laser"])

    def _show_status_message(self, message: str) -> None:
        self.statusBar().showMessage(message, 3500)

    def closeEvent(self, event) -> None:
        self._disconnect_active(log_if_idle=False)
        super().closeEvent(event)

    def _build_monitor_page(self) -> QWidget:
        page = self._page_shell("Monitor", "Live device status placeholder.")
        grid = QGridLayout()
        grid.setHorizontalSpacing(16)
        grid.setVerticalSpacing(16)
        cards = (("RX", "DISCONNECTED"), ("TX", "DISCONNECTED"), ("SLE", "IDLE"), ("Laser", "OFF"))
        for index, (name, value) in enumerate(cards):
            card = QFrame()
            card.setObjectName("monitorCard")
            card_layout = QVBoxLayout(card)
            label = QLabel(name)
            label.setObjectName("cardLabel")
            state = QLabel(value)
            state.setObjectName("cardValue")
            card_layout.addWidget(label)
            card_layout.addWidget(state)
            grid.addWidget(card, index // 2, index % 2)
        page.layout().addLayout(grid)
        page.layout().addStretch(1)
        return page

    def _build_placeholder_page(self, title: str, subtitle: str) -> QWidget:
        page = self._page_shell(title, subtitle)
        placeholder = QFrame()
        placeholder.setObjectName("placeholder")
        placeholder_layout = QVBoxLayout(placeholder)
        label = QLabel("Not implemented in V1")
        label.setObjectName("placeholderTitle")
        detail = QLabel("This page is reserved so navigation and product layout stay stable while transport work continues.")
        detail.setWordWrap(True)
        placeholder_layout.addWidget(label)
        placeholder_layout.addWidget(detail)
        placeholder_layout.addStretch(1)
        page.layout().addWidget(placeholder, 1)
        return page

    def _page_shell(self, title: str, subtitle: str) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(28, 24, 28, 28)
        layout.setSpacing(18)
        header = QLabel(title)
        header.setObjectName("pageTitle")
        caption = QLabel(subtitle)
        caption.setObjectName("pageCaption")
        caption.setWordWrap(True)
        layout.addWidget(header)
        layout.addWidget(caption)
        return page

    def _apply_style(self) -> None:
        self.setStyleSheet(
            """
            QMainWindow { background: #f4f6f8; color: #17202a; }
            #statusStrip { background: #111827; border-bottom: 1px solid #273244; }
            #appTitle { color: #f9fafb; font-size: 18px; font-weight: 700; }
            QLabel[badge="true"] {
                color: #e5e7eb; background: #253044; border: 1px solid #3a465c;
                border-radius: 6px; padding: 6px 10px; font-weight: 600;
            }
            #nav {
                background: #17202a; color: #d8dee9; border: 0; padding: 12px 8px; font-size: 14px;
            }
            #nav::item { min-height: 40px; padding-left: 12px; border-radius: 6px; }
            #nav::item:selected { background: #2f80ed; color: white; }
            #pageTitle { font-size: 26px; font-weight: 700; color: #111827; }
            #pageCaption { color: #5b6472; font-size: 13px; }
            QGroupBox {
                background: white; border: 1px solid #d9dee7; border-radius: 8px;
                margin-top: 12px; padding: 18px; font-weight: 700;
            }
            QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; }
            QPushButton {
                background: #2f80ed; color: white; border: 0; border-radius: 6px;
                padding: 8px 14px; font-weight: 600;
            }
            QPushButton:hover { background: #256ed1; }
            QLineEdit, QComboBox, QSpinBox {
                background: white; border: 1px solid #cfd6e1; border-radius: 6px;
                min-height: 30px; padding: 4px 8px;
            }
            #logView {
                background: #0f172a; color: #dbeafe; border: 1px solid #1f2a44;
                border-radius: 8px; font-family: Consolas, "Courier New", monospace; font-size: 12px;
            }
            #placeholder, #monitorCard {
                background: white; border: 1px solid #d9dee7; border-radius: 8px; padding: 18px;
            }
            #placeholderTitle { font-size: 18px; font-weight: 700; }
            #cardLabel { color: #6b7280; font-size: 13px; }
            #cardValue { color: #111827; font-size: 24px; font-weight: 700; }
            """
        )
        for page in range(self.pages.count()):
            widget = self.pages.widget(page)
            widget.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
