from __future__ import annotations

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtGui import QColor, QDoubleValidator, QIntValidator, QPainter, QPen
from PySide6.QtWidgets import (
    QFrame, QGridLayout, QHBoxLayout, QLabel, QLineEdit,
    QProgressBar, QPushButton, QSizePolicy, QVBoxLayout, QWidget, QGroupBox,
)

from app.config_store import HostConfig


class ArcWidget(QWidget):
    """Custom painted arc progress indicator (ring gauge)."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setFixedSize(140, 140)
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self._value = 0
        self._max = 100
        self._color = QColor("#0284C7")
        self._track_color = QColor("#E2E8F0")
        self._caption = "等待任务"
        self._spinning = False
        self._spin_angle = 0
        self._spin_timer = QTimer(self)
        self._spin_timer.setInterval(40)
        self._spin_timer.timeout.connect(self._advance_spinner)

    def set_value(self, v: int) -> None:
        self._value = max(0, min(self._max, v))
        self.update()

    def set_color(self, hex_color: str) -> None:
        self._color = QColor(hex_color)
        self.update()

    def set_caption(self, caption: str) -> None:
        self._caption = caption
        self.update()

    def set_spinning(self, active: bool) -> None:
        if self._spinning == active:
            return
        self._spinning = active
        if active:
            self._spin_timer.start()
        else:
            self._spin_timer.stop()
            self._spin_angle = 0
        self.update()

    def _advance_spinner(self) -> None:
        self._spin_angle = (self._spin_angle + 5) % 360
        self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        
        margin = 10
        rect = self.rect().adjusted(margin, margin, -margin, -margin)

        # Track
        pen = QPen(self._track_color, 10, Qt.SolidLine, Qt.RoundCap)
        p.setPen(pen)
        p.drawArc(rect, 0, 360 * 16)

        # Execution uses a rotating, open ring; upload uses determinate progress.
        if self._spinning:
            start = int((90 - self._spin_angle) * 16)
            span = -int(275 * 16)

            glow_color = QColor(self._color.red(), self._color.green(), self._color.blue(), 35)
            glow_pen = QPen(glow_color, 15, Qt.SolidLine, Qt.RoundCap)
            p.setPen(glow_pen)
            p.drawArc(rect, start, span)

            pen.setColor(self._color)
            pen.setWidth(10)
            p.setPen(pen)
            p.drawArc(rect, start, span)
        elif self._value > 0:
            span = int(self._value * 360 * 16 / self._max)
            
            # Glow arc (semi-transparent, wider)
            glow_color = QColor(self._color.red(), self._color.green(), self._color.blue(), 30)
            glow_pen = QPen(glow_color, 15, Qt.SolidLine, Qt.RoundCap)
            p.setPen(glow_pen)
            p.drawArc(rect, 90 * 16, -span)

            # Solid value arc
            pen.setColor(self._color)
            pen.setWidth(10)
            p.setPen(pen)
            p.drawArc(rect, 90 * 16, -span)

        # Center text percentage
        p.setPen(QColor("#0F172A"))
        font = p.font()
        font.setPixelSize(27)
        font.setBold(True)
        p.setFont(font)
        center_text = "执行中" if self._spinning else f"{self._value}%"
        p.drawText(rect.adjusted(0, -10, 0, 0), Qt.AlignCenter, center_text)

        # Center subtitle
        p.setPen(QColor("#475569"))
        font.setPixelSize(11)
        font.setBold(False)
        p.setFont(font)
        p.drawText(rect.adjusted(0, 32, 0, 0), Qt.AlignCenter, self._caption)

        p.end()


class JobPage(QWidget):
    """Job upload, execution, control, and RX status dashboard."""

    upload_requested = Signal()             # upload only
    upload_exec_requested = Signal()        # upload + execute
    exec_start_requested = Signal()
    exec_stop_requested = Signal()
    exec_resume_requested = Signal()
    abort_requested = Signal()
    outline_scan_requested = Signal()
    focus_on_requested = Signal(int)        # power 0-100
    focus_off_requested = Signal()
    status_requested = Signal()
    switch_wifi_requested = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._focus_active = False
        self._execution_active = False
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setSpacing(10)
        layout.setContentsMargins(24, 16, 24, 16)

        subtitle = QLabel("管理仅上传任务和上传并执行任务，查看接收端状态，控制暂停、恢复、取消及调焦光。")
        subtitle.setObjectName("pageSubtitle")
        subtitle.setWordWrap(True)
        layout.addWidget(subtitle)

        # 2-Column Dashboard Layout (Left: Monitoring, Right: Controls)
        dashboard_layout = QHBoxLayout()
        dashboard_layout.setSpacing(12)

        # ==================== LEFT COLUMN: MONITORING ====================
        left_column = QVBoxLayout()
        left_column.setSpacing(10)

        # 1. Circular Gauge Card
        left_card = QFrame()
        left_card.setObjectName("monitorCard")
        left_card.setFixedWidth(230)
        left_card_layout = QVBoxLayout(left_card)
        left_card_layout.setSpacing(6)
        left_card_layout.setContentsMargins(12, 10, 12, 10)

        self.arc = ArcWidget()
        left_card_layout.addWidget(self.arc, 0, Qt.AlignCenter)

        self.lbl_state = QLabel("待机")
        self.lbl_state.setObjectName("stateLabel")
        self.lbl_state.setAlignment(Qt.AlignCenter)
        left_card_layout.addWidget(self.lbl_state)

        self.lbl_substate = QLabel("待机模式")
        self.lbl_substate.setObjectName("substateLabel")
        self.lbl_substate.setAlignment(Qt.AlignCenter)
        left_card_layout.addWidget(self.lbl_substate)

        left_column.addWidget(left_card)

        # 2. Readback status cards (4x2 grid)
        self.cards: dict[str, QLabel] = {}
        grid_status = QGridLayout()
        grid_status.setSpacing(6)

        card_defs = [
            ("接收端状态", "待机"),
            ("当前任务 ID", "--"),
            ("已接收字节", "--"),
            ("任务总字节", "--"),
            ("缓存剩余", "--"),
            ("调焦指示", "关闭"),
            ("TX 命令串口", "未连接"),
            ("RX 日志监控", "未连接"),
        ]

        for i, (name, default) in enumerate(card_defs):
            card = QFrame()
            card.setObjectName("monitorCard")
            cl = QVBoxLayout(card)
            cl.setSpacing(2)
            cl.setContentsMargins(8, 5, 8, 5)

            lbl_name = QLabel(name)
            lbl_name.setObjectName("cardLabel")
            lbl_name.setStyleSheet("font-size: 10px;")

            lbl_val = QLabel(default)
            lbl_val.setObjectName("cardValue")
            lbl_val.setStyleSheet("font-size: 15px;")

            cl.addWidget(lbl_name)
            cl.addWidget(lbl_val)
            grid_status.addWidget(card, i // 2, i % 2)
            self.cards[name] = lbl_val

        left_column.addLayout(grid_status)
        dashboard_layout.addLayout(left_column, 0)

        # ==================== RIGHT COLUMN: CONTROL CARDS ====================
        right_column = QGridLayout()
        right_column.setSpacing(10)

        # Card 1: 任务参数与上传
        param_group = QGroupBox("任务参数与上传")
        p_layout = QVBoxLayout(param_group)
        p_layout.setSpacing(8)
        p_layout.setContentsMargins(14, 20, 14, 10)

        param_inputs = QHBoxLayout()
        param_inputs.setSpacing(8)

        vbox_jid = QVBoxLayout()
        vbox_jid.setSpacing(3)
        vbox_jid.addWidget(QLabel("任务 ID"))
        self.job_id_edit = QLineEdit("1")
        self.job_id_edit.setValidator(QIntValidator(1, 2_147_483_647, self))
        self.job_id_edit.setMinimumHeight(32)
        vbox_jid.addWidget(self.job_id_edit)
        param_inputs.addLayout(vbox_jid, 1)

        vbox_timeout = QVBoxLayout()
        vbox_timeout.setSpacing(3)
        vbox_timeout.addWidget(QLabel("超时时长 (s)"))
        self.timeout_edit = QLineEdit("20")
        self.timeout_edit.setValidator(QDoubleValidator(1.0, 3600.0, 2, self))
        self.timeout_edit.setMinimumHeight(32)
        vbox_timeout.addWidget(self.timeout_edit)
        param_inputs.addLayout(vbox_timeout, 1)

        p_layout.addLayout(param_inputs)

        vbox_preroll = QVBoxLayout()
        vbox_preroll.setSpacing(3)
        vbox_preroll.addWidget(QLabel("执行预缓冲字节数（仅用于上传并执行）"))
        self.preroll_edit = QLineEdit("4096")
        self.preroll_edit.setValidator(QIntValidator(0, 100_000_000, self))
        self.preroll_edit.setMinimumHeight(32)
        vbox_preroll.addWidget(self.preroll_edit)
        p_layout.addLayout(vbox_preroll)

        p_btn_row = QHBoxLayout()
        p_btn_row.setSpacing(8)
        self.btn_upload = QPushButton("仅上传任务")
        self.btn_upload.setObjectName("btnPrimary")
        self.btn_upload.setCursor(self.cursor())
        self.btn_upload.setMinimumHeight(34)
        self.btn_upload.clicked.connect(self.upload_requested.emit)
        p_btn_row.addWidget(self.btn_upload, 1)

        self.btn_upload_exec = QPushButton("上传并执行")
        self.btn_upload_exec.setObjectName("btnAccent")
        self.btn_upload_exec.setCursor(self.cursor())
        self.btn_upload_exec.setMinimumHeight(34)
        self.btn_upload_exec.clicked.connect(self.upload_exec_requested.emit)
        p_btn_row.addWidget(self.btn_upload_exec, 1)
        p_layout.addLayout(p_btn_row)

        right_column.addWidget(param_group, 0, 0)

        # Card 2: 上传状态
        prog_group = QGroupBox("上传状态")
        pr_layout = QVBoxLayout(prog_group)
        pr_layout.setSpacing(7)
        pr_layout.setContentsMargins(14, 20, 14, 10)

        status_row = QHBoxLayout()
        self.lbl_task = QLabel("空闲")
        self.lbl_task.setObjectName("taskLabel")
        status_row.addWidget(self.lbl_task)
        status_row.addStretch(1)
        self.lbl_progress_pct = QLabel("0%")
        self.lbl_progress_pct.setStyleSheet("color:#0284c7; font-size:12px; font-weight:700;")
        status_row.addWidget(self.lbl_progress_pct)
        pr_layout.addLayout(status_row)
        
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        self.progress.setTextVisible(False)
        pr_layout.addWidget(self.progress)

        summary = QFrame()
        summary.setObjectName("progressSummary")
        summary.setStyleSheet(
            "QFrame#progressSummary { background:#f8fafc; border:1px solid #e2e8f0; "
            "border-radius:8px; }"
        )
        summary_layout = QVBoxLayout(summary)
        summary_layout.setContentsMargins(8, 6, 8, 6)
        summary_layout.setSpacing(2)
        summary_title = QLabel("串口任务输出摘要:")
        summary_title.setStyleSheet("font-size:12px; font-weight:700; color:#475569; border:none;")
        summary_layout.addWidget(summary_title)
        self.lbl_progress_detail = QLabel("暂无正在进行的任务")
        self.lbl_progress_detail.setWordWrap(True)
        self.lbl_progress_detail.setStyleSheet("font-size:12px; font-weight:600; color:#0284c7; border:none;")
        summary_layout.addWidget(self.lbl_progress_detail)
        pr_layout.addWidget(summary)

        right_column.addWidget(prog_group, 0, 1)

        # Card 3: 仅上传任务控制
        manual_group = QGroupBox("仅上传任务控制")
        e_layout = QVBoxLayout(manual_group)
        e_layout.setSpacing(7)
        e_layout.setContentsMargins(14, 20, 14, 10)

        self.btn_exec = QPushButton("开始执行")
        self.btn_exec.setObjectName("btnAccent")
        self.btn_exec.setCursor(self.cursor())
        self.btn_exec.setMinimumHeight(34)
        self.btn_exec.clicked.connect(self.exec_start_requested.emit)
        e_layout.addWidget(self.btn_exec)

        self.btn_abort = QPushButton("清空已上传队列")
        self.btn_abort.setObjectName("btnDanger")
        self.btn_abort.setCursor(self.cursor())
        self.btn_abort.setMinimumHeight(34)
        self.btn_abort.clicked.connect(self.abort_requested.emit)
        e_layout.addWidget(self.btn_abort)

        e_layout.addStretch(1)
        right_column.addWidget(manual_group, 1, 0)

        # Card 4: 上传并执行控制
        live_group = QGroupBox("上传并执行控制")
        live_layout = QVBoxLayout(live_group)
        live_layout.setSpacing(7)
        live_layout.setContentsMargins(14, 20, 14, 10)

        self.btn_stop = QPushButton("暂停执行")
        self.btn_stop.setObjectName("btnWarn")
        self.btn_stop.setCursor(self.cursor())
        self.btn_stop.setMinimumHeight(34)
        self.btn_stop.clicked.connect(self.exec_stop_requested.emit)
        live_layout.addWidget(self.btn_stop)

        self.btn_resume = QPushButton("恢复执行")
        self.btn_resume.setObjectName("btnAccent")
        self.btn_resume.setCursor(self.cursor())
        self.btn_resume.setMinimumHeight(34)
        self.btn_resume.clicked.connect(self.exec_resume_requested.emit)
        live_layout.addWidget(self.btn_resume)

        self.btn_force_cancel = QPushButton("强制断光并取消任务")
        self.btn_force_cancel.setObjectName("btnDanger")
        self.btn_force_cancel.setCursor(self.cursor())
        self.btn_force_cancel.setMinimumHeight(34)
        self.btn_force_cancel.clicked.connect(self.abort_requested.emit)
        live_layout.addWidget(self.btn_force_cancel)

        live_layout.addStretch(1)
        right_column.addWidget(live_group, 1, 1)

        # Card 5: 状态查询与调焦
        safety_group = QGroupBox("状态查询与调焦")
        s_layout = QVBoxLayout(safety_group)
        s_layout.setSpacing(8)
        s_layout.setContentsMargins(14, 20, 14, 10)

        self.btn_status = QPushButton("查询状态")
        self.btn_status.setCursor(self.cursor())
        self.btn_status.setMinimumHeight(34)
        self.btn_status.clicked.connect(self.status_requested.emit)
        s_layout.addWidget(self.btn_status)

        self.btn_outline_scan = QPushButton("扫描外框")
        self.btn_outline_scan.setObjectName("btnWarn")
        self.btn_outline_scan.setCursor(self.cursor())
        self.btn_outline_scan.setMinimumHeight(34)
        self.btn_outline_scan.setToolTip("按当前 G-code 的实际开光范围低功率高速扫描两圈")
        self.btn_outline_scan.clicked.connect(self.outline_scan_requested.emit)
        s_layout.addWidget(self.btn_outline_scan)

        # Divider
        divider = QWidget()
        divider.setMinimumHeight(1)
        divider.setMaximumHeight(1)
        divider.setStyleSheet("background-color: #e2e8f0;")
        s_layout.addWidget(divider)

        # Focus row
        focus_layout = QHBoxLayout()
        focus_layout.setSpacing(8)
        
        lbl_fpow = QLabel("弱光功率 (0-100)")
        lbl_fpow.setFixedWidth(100)
        focus_layout.addWidget(lbl_fpow)
        
        self.focus_power = QLineEdit("10")
        self.focus_power.setValidator(QIntValidator(0, 100, self))
        self.focus_power.setMaximumWidth(60)
        self.focus_power.setMinimumHeight(30)
        self.focus_power.setAlignment(Qt.AlignCenter)
        focus_layout.addWidget(self.focus_power)
        
        self.btn_focus = QPushButton("开启调焦")
        self.btn_focus.setObjectName("btnFocus")
        self.btn_focus.setProperty("active", False)
        self.btn_focus.setCursor(self.cursor())
        self.btn_focus.setMinimumHeight(30)
        self.btn_focus.clicked.connect(self._on_focus)
        focus_layout.addWidget(self.btn_focus, 1)
        s_layout.addLayout(focus_layout)

        right_column.addWidget(safety_group, 2, 0)

        # Card 6: 路由切换
        route_group = QGroupBox("路由切换")
        route_group.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        r_layout = QVBoxLayout(route_group)
        r_layout.setSpacing(5)
        r_layout.setContentsMargins(14, 18, 14, 8)

        route_desc = QLabel("将接收板的指令通信路由切换至 Wi-Fi / GRBL 无线控制旁路。")
        route_desc.setWordWrap(True)
        route_desc.setStyleSheet("color: #475569; font-size: 13px;")
        r_layout.addWidget(route_desc)

        self.btn_wifi = QPushButton("切换至 Wi-Fi / GRBL 控制")
        self.btn_wifi.setObjectName("btnPrimary")
        self.btn_wifi.setCursor(self.cursor())
        self.btn_wifi.setMinimumHeight(32)
        self.btn_wifi.clicked.connect(self.switch_wifi_requested.emit)
        r_layout.addWidget(self.btn_wifi)

        right_column.addWidget(route_group, 2, 1)

        dashboard_layout.addLayout(right_column, 1)

        layout.addLayout(dashboard_layout, 1)

    def load_config(self, config: HostConfig) -> None:
        self.job_id_edit.setText(str(config.job_id))
        self.timeout_edit.setText(str(config.timeout_cmd))
        self.preroll_edit.setText(str(config.preroll_bytes))
        self.focus_power.setText(str(config.focus_power))

    def get_job_id(self) -> int:
        try:
            return max(1, int(self.job_id_edit.text().strip()))
        except ValueError:
            return 1

    def get_timeout(self) -> float:
        try:
            return max(1.0, float(self.timeout_edit.text().strip()))
        except ValueError:
            return 20.0

    def get_preroll(self) -> int:
        try:
            return max(0, int(self.preroll_edit.text().strip()))
        except ValueError:
            return 4096

    def set_task_status(self, text: str, progress_pct: float = -1) -> None:
        self.lbl_task.setText(text)
        self.lbl_progress_detail.setText(text)
        if 0 <= progress_pct <= 100:
            self.progress.setValue(int(progress_pct))
            self.lbl_progress_pct.setText(f"{int(progress_pct)}%")

    def set_progress_text(self, text: str) -> None:
        self.lbl_progress_detail.setText(text)

    def set_focus_state(self, active: bool) -> None:
        self._focus_active = active
        self.btn_focus.setText("关闭调焦" if active else "开启调焦")
        self.btn_focus.setProperty("active", active)
        self.btn_focus.style().unpolish(self.btn_focus)
        self.btn_focus.style().polish(self.btn_focus)
        self.btn_focus.update()

    def set_execution_state(self, active: bool, *, completed: bool = False) -> None:
        self._execution_active = active
        self.arc.set_spinning(active)
        if active:
            self.arc.set_color("#EF4444")
            self.arc.set_caption("等待 RX 完成")
            self.lbl_state.setText("正在执行")
            self.lbl_state.setStyleSheet("color: #EF4444; font-size: 24px; font-weight: 800;")
            self.lbl_substate.setText("激光器高速打标中")
        elif completed:
            self.arc.set_value(100)
            self.arc.set_color("#10B981")
            self.arc.set_caption("执行完成")
            self.lbl_state.setText("执行完成")
            self.lbl_state.setStyleSheet("color: #10B981; font-size: 24px; font-weight: 800;")
            self.lbl_substate.setText("任务已完成，控制已释放")

    def _on_focus(self) -> None:
        if self._focus_active:
            self.focus_off_requested.emit()
        else:
            try:
                power = max(0, min(100, int(self.focus_power.text().strip())))
            except ValueError:
                power = 10
            self.focus_on_requested.emit(power)

    def update_state(self, rx_state: str, rx_state_code: int, job_id: int,
                     received: int, total: int, cache_free: int,
                     focus: str, tx_link: str, rx_link: str) -> None:
        """Called by MainWindow to update system telemetry and visualization."""
        if rx_state_code == 1 and total > 0:
            calc_progress = max(0, min(100, int(received * 100 / total)))
            progress_caption = "下载进度"
            phase_detail = f"下载 {received}/{total} 字节"
        elif rx_state_code == 2:
            calc_progress = 100
            progress_caption = "下载完成"
            phase_detail = "任务就绪，等待执行"
        elif rx_state_code == 3:
            calc_progress = 0
            progress_caption = "正在执行"
            phase_detail = "任务正在执行"
        else:
            calc_progress = 0
            progress_caption = "等待任务"
            phase_detail = "待机模式"

        self.cards["接收端状态"].setText(rx_state)
        self.cards["当前任务 ID"].setText(str(job_id) if job_id > 0 else "--")
        self.cards["已接收字节"].setText(f"{received} 字节" if total > 0 else "--")
        self.cards["任务总字节"].setText(f"{total} 字节" if total > 0 else "--")
        self.cards["缓存剩余"].setText(f"{cache_free} 字节")

        # Focus state styling
        focus_cn = "开启" if focus.startswith("ON") else "关闭"
        self.cards["调焦指示"].setText(focus_cn)
        if focus.startswith("ON"):
            self.cards["调焦指示"].setStyleSheet("color: #D97706;")
        else:
            self.cards["调焦指示"].setStyleSheet("color: #475569;")

        # Connection Links styling
        link_map = {
            "CONNECTED": "已连接",
            "DISCONNECTED": "未连接",
            "CONNECTING": "连接中",
            "ERROR": "故障"
        }
        tx_cn = link_map.get(tx_link, tx_link)
        rx_cn = link_map.get(rx_link, rx_link)
        self.cards["TX 命令串口"].setText(tx_cn)
        self.cards["TX 命令串口"].setStyleSheet("color: #10B981;" if tx_link == "CONNECTED" else "color: #EF4444;")
        self.cards["RX 日志监控"].setText(rx_cn)
        self.cards["RX 日志监控"].setStyleSheet("color: #10B981;" if rx_link == "CONNECTED" else "color: #EF4444;")

        self.lbl_state.setText(rx_state)
        self.arc.set_value(calc_progress)
        self.arc.set_caption(progress_caption)

        # Color coding state label and ring color
        color_map = {
            "待机": ("#0284C7", "待机模式"),
            "空闲": ("#0284C7", "待机模式"),
            "数据传输中": ("#2563EB", "正在下载任务数据到内存"),
            "任务就绪": ("#10B981", "代码已被成功读取且校验通过"),
            "正在执行": ("#EF4444", "激光器高速打标中"),
            "已终止": ("#EF4444", "已终止"),
            "错误": ("#EF4444", "警报/故障"),
        }
        color, _sub = color_map.get(rx_state, ("#94A3B8", "STANDBY"))
        self.arc.set_color(color)
        self.lbl_substate.setText(phase_detail)
        self.lbl_state.setStyleSheet(f"color: {color}; font-size: 24px; font-weight: 800;")
