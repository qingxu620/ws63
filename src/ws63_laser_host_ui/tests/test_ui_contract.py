from __future__ import annotations

import io
import os
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QColor, QImage
from PySide6.QtWidgets import QApplication, QLabel, QGroupBox, QScrollArea, QTabWidget

from app.config_store import HostConfig
from app.state_models import AppState
from app.state_models import LinkState
from transports.sle_tx_transport import JOB_MAX_SIZE, JOB_PREROLL_BYTES
from ui.main_window import MainWindow
from ui.pages.connection_page import ConnectionPage
from ui.pages.gcode_page import GcodePage
from ui.pages.job_page import JobPage
from ui.pages.logs_page import LogsPage
from ui.widgets.sidebar_nav import SidebarNav


class UiContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    def test_main_window_exposes_four_mockup_pages_and_brand(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)

        self.assertEqual(window.pages.count(), 4)
        brand = window.findChild(QLabel, "headerBrand")
        self.assertIsNotNone(brand)
        self.assertIn("WS63", brand.text())

    def test_sidebar_uses_compact_width(self) -> None:
        sidebar = SidebarNav()

        self.assertLessEqual(sidebar.width(), 176)

    def test_disconnected_monitor_uses_a_human_readable_standby_state(self) -> None:
        self.assertEqual(AppState().rx_state_name, "待机")

    def test_connection_page_uses_fixed_two_column_card_grid(self) -> None:
        page = ConnectionPage()

        self.assertIsNone(page.findChild(QScrollArea, "pageScroll"))
        self.assertEqual(page.cards_grid.columnCount(), 2)

    def test_all_pages_omit_large_page_titles(self) -> None:
        pages = (ConnectionPage(), GcodePage(), JobPage(), LogsPage())

        for page in pages:
            self.assertIsNone(page.findChild(QLabel, "pageTitle"))

    def test_connection_and_job_bottom_actions_fit_at_minimum_window_size(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.resize(1100, 700)
        window.show()
        self.app.processEvents()

        connection_bottom = window.page_conn.btn_save.mapTo(
            window.page_conn, QPoint(0, window.page_conn.btn_save.height())
        ).y()
        self.assertLessEqual(connection_bottom, window.page_conn.height())

        window.sidebar.set_current(2)
        self.app.processEvents()
        route_bottom = window.page_job.btn_wifi.mapTo(
            window.page_job, QPoint(0, window.page_job.btn_wifi.height())
        ).y()
        self.assertLessEqual(route_bottom, window.page_job.height())
        self.assertIsNone(window.page_job.findChild(QScrollArea, "pageScroll"))

    def test_gcode_page_contains_ai_and_editor_tabs(self) -> None:
        page = GcodePage()

        tabs = page.findChild(QTabWidget, "gcodeTabs")
        self.assertIsNotNone(tabs)
        self.assertEqual(tabs.count(), 2)
        self.assertEqual(tabs.tabText(0), "AI 生图与矢量化")
        self.assertEqual(tabs.tabText(1), "G-code 编辑器")

    def test_ai_controls_have_independent_vertical_scroll_protection(self) -> None:
        page = GcodePage()

        controls_scroll = page.findChild(QScrollArea, "aiControlsScroll")

        self.assertIsNotNone(controls_scroll)
        self.assertEqual(
            controls_scroll.horizontalScrollBarPolicy(),
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff,
        )

    def test_builtin_prompt_templates_follow_conversion_mode(self) -> None:
        page = GcodePage()

        scanline_prompt = page._compose_generation_prompt("一只皮卡丘")
        self.assertIn("一只皮卡丘", scanline_prompt)
        self.assertIn("扫描填充", scanline_prompt)
        self.assertIn("明暗层次", scanline_prompt)
        self.assertIn("允许灰度", scanline_prompt)

        page.extract_mode.setCurrentIndex(page.extract_mode.findData("vector"))
        vector_prompt = page._compose_generation_prompt("一只皮卡丘")
        self.assertIn("轮廓矢量", vector_prompt)
        self.assertIn("主体边界清晰", vector_prompt)
        self.assertIn("主要内轮廓", vector_prompt)
        self.assertNotIn("黑白线稿", vector_prompt)

        page.extract_mode.setCurrentIndex(page.extract_mode.findData("lasergrbl_vector"))
        lasergrbl_prompt = page._compose_generation_prompt("一只皮卡丘")
        self.assertIn("轮廓矢量", lasergrbl_prompt)
        self.assertIn("主体边界清晰", lasergrbl_prompt)

    def test_raw_prompt_template_keeps_user_prompt_unchanged(self) -> None:
        page = GcodePage()
        page.prompt_preset_combo.setCurrentIndex(page.prompt_preset_combo.findData("raw"))

        self.assertEqual(page._compose_generation_prompt("生成一只皮卡丘"), "生成一只皮卡丘")

    def test_imported_image_converts_into_the_existing_editor_flow(self) -> None:
        page = GcodePage()
        image = QImage(4, 2, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        image.setPixelColor(0, 0, QColor("black"))
        emitted: list[tuple[str, bytes]] = []
        page.gcode_loaded.connect(lambda path, data: emitted.append((path, data)))

        page.set_image("sample.png", image)
        page.btn_convert.click()

        self.assertEqual(page.tabs.currentIndex(), 0)
        self.assertTrue(emitted)
        self.assertEqual(emitted[-1][0], "sample.scanline.generated.gcode")
        self.assertIn(b"M4 S0", emitted[-1][1])
        self.assertNotIn(b"M30", emitted[-1][1])
        self.assertTrue(page.btn_preview_converted.isEnabled())
        self.assertTrue(page.btn_preview_converted.isChecked())
        converted_preview_key = page.preview_label.pixmap().cacheKey()

        page.btn_preview_original.click()

        self.assertTrue(page.btn_preview_original.isChecked())
        self.assertNotEqual(
            page.preview_label.pixmap().cacheKey(), converted_preview_key
        )

    def test_image_conversion_mode_can_switch_to_vector_outline(self) -> None:
        page = GcodePage()
        image = QImage(8, 8, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        for y in range(2, 6):
            for x in range(2, 6):
                image.setPixelColor(x, y, QColor("black"))
        emitted: list[tuple[str, bytes]] = []
        page.gcode_loaded.connect(lambda path, data: emitted.append((path, data)))

        self.assertEqual(page.extract_mode.currentData(), "scanline")
        self.assertFalse(page.vector_simplify.isEnabled())
        page.extract_mode.setCurrentIndex(page.extract_mode.findData("vector"))
        self.assertTrue(page.vector_simplify.isEnabled())
        self.assertEqual(page.vector_simplify.value(), 1.0)
        page.set_image("outline.png", image)
        page.btn_convert.click()

        self.assertTrue(emitted)
        self.assertEqual(emitted[-1][0], "outline.vector.generated.gcode")
        self.assertIn(b"M4 S0", emitted[-1][1])
        self.assertIn(b"S1000", emitted[-1][1])
        self.assertNotIn(b"M3", emitted[-1][1])
        self.assertIn("轮廓矢量", page.image_status.text())
        self.assertIn("路径", page.image_status.text())

    def test_image_conversion_mode_can_use_lasergrbl_style_vector(self) -> None:
        page = GcodePage()
        image = QImage(8, 8, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        for y in range(2, 6):
            for x in range(2, 6):
                image.setPixelColor(x, y, QColor("black"))
        emitted: list[tuple[str, bytes]] = []
        page.gcode_loaded.connect(lambda path, data: emitted.append((path, data)))

        page.extract_mode.setCurrentIndex(page.extract_mode.findData("lasergrbl_vector"))
        self.assertTrue(page.vector_simplify.isEnabled())
        page.set_image("outline.png", image)
        page.btn_convert.click()

        self.assertTrue(emitted)
        self.assertEqual(emitted[-1][0], "outline.lasergrbl_vector.generated.gcode")
        self.assertIn(b"M4 S0", emitted[-1][1])
        self.assertIn(b"S1000", emitted[-1][1])
        self.assertIn("LaserGRBL", page.image_status.text())
        self.assertIn("白场", page.image_status.text())
        self.assertIn("路径", page.image_status.text())

    def test_image_conversion_uses_selected_mark_power(self) -> None:
        page = GcodePage()
        image = QImage(4, 2, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        image.setPixelColor(0, 0, QColor("black"))
        emitted: list[tuple[str, bytes]] = []
        page.gcode_loaded.connect(lambda path, data: emitted.append((path, data)))

        page.power_spin.setValue(350)
        page.set_image("power.png", image)
        page.btn_convert.click()

        self.assertIn(b"S350", emitted[-1][1])
        self.assertNotIn(b"S1000", emitted[-1][1])
        self.assertIn("S350", page.image_status.text())

    def test_outline_scan_payload_builds_two_loop_low_power_job(self) -> None:
        page = GcodePage()
        image = QImage(8, 8, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        for y in range(2, 6):
            for x in range(2, 6):
                image.setPixelColor(x, y, QColor("black"))

        page.extract_mode.setCurrentIndex(page.extract_mode.findData("vector"))
        page.set_image("outline.png", image)
        page.btn_convert.click()

        payload = page.build_outline_scan_payload()

        self.assertIsNotNone(payload)
        assert payload is not None
        path, data = payload
        self.assertTrue(path.endswith(".outline_scan.gcode"))
        text = data.decode("utf-8")
        self.assertIn("F10000", text)
        self.assertIn("M4 S0", text)
        self.assertIn("S80", text)
        self.assertEqual(text.count("\nG1 "), 8)
        self.assertTrue(text.endswith("S0\nM5\n"))
        self.assertIn("2圈", page.image_status.text())

    def test_outline_scan_button_lives_on_job_control_page(self) -> None:
        page = JobPage()

        self.assertTrue(hasattr(page, "btn_outline_scan"))
        self.assertEqual(page.btn_outline_scan.text(), "扫描外框")
        self.assertFalse(hasattr(GcodePage(), "btn_outline_scan"))

    def test_outline_scan_request_uses_existing_upload_execute_worker(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        captured: list[tuple[object, tuple[object, ...]]] = []
        window.client.is_open = lambda: True
        window._run_job_worker = lambda fn, *args: captured.append((fn, args))
        window.page_gcode.set_content(
            "shape.gcode",
            b"G90\nM5\nG0 X0 Y0\nM3 S80\nG1 X1 Y0\nG1 X1 Y1\nG1 X0 Y1\nG1 X0 Y0\nM5\n",
        )

        window._on_outline_scan_requested()

        self.assertTrue(captured)
        self.assertEqual(captured[-1][0].__name__, "_upload_exec_worker")
        self.assertIn(b"F10000", captured[-1][1][0])
        self.assertEqual(captured[-1][1][3], 0)
        self.assertTrue(window.state.gcode_path.endswith(".outline_scan.gcode"))

    def test_command_port_uses_single_stateful_connect_button(self) -> None:
        page = ConnectionPage()

        self.assertFalse(hasattr(page, "btn_disconnect"))
        self.assertEqual(page.btn_connect.text(), "连接")
        self.assertEqual(page.btn_connect.objectName(), "btnPrimary")

        page.set_connected(True)
        self.assertEqual(page.btn_connect.text(), "断开连接")
        self.assertEqual(page.btn_connect.objectName(), "btnDanger")
        self.assertFalse(page.cmd_combo.isEnabled())

        page.set_connected(False)
        self.assertEqual(page.btn_connect.text(), "连接")
        self.assertEqual(page.btn_connect.objectName(), "btnPrimary")
        self.assertTrue(page.cmd_combo.isEnabled())

    def test_marking_range_is_limited_to_99mm_square(self) -> None:
        page = GcodePage()

        self.assertEqual(page.size_spin.minimum(), 0)
        self.assertEqual(page.size_spin.maximum(), 99)
        self.assertEqual(page.size_spin.value(), 99)

    def test_zero_square_size_generates_safe_empty_job_in_both_modes(self) -> None:
        page = GcodePage()
        image = QImage(8, 8, QImage.Format.Format_RGB32)
        image.fill(QColor("black"))
        emitted: list[tuple[str, bytes]] = []
        page.gcode_loaded.connect(lambda path, data: emitted.append((path, data)))
        page.set_image("zero.png", image)
        page.size_spin.setValue(0)

        for mode in ("scanline", "vector", "lasergrbl_vector"):
            page.extract_mode.setCurrentIndex(page.extract_mode.findData(mode))
            page.btn_convert.click()
            self.assertEqual(emitted[-1][1], b"M4 S0\nM5\n")
            self.assertNotIn(b"G0 ", emitted[-1][1])
            self.assertNotIn(b"G1 ", emitted[-1][1])
            self.assertNotIn(b"M3 S", emitted[-1][1])
            self.assertNotIn(b"M30", emitted[-1][1])
            self.assertIn("0×0 mm", page.image_status.text())

    def test_extreme_portrait_image_is_bounded_before_conversion(self) -> None:
        page = GcodePage()
        image = QImage(1, 1000, QImage.Format.Format_RGB32)
        image.fill(QColor("black"))
        emitted: list[tuple[str, bytes]] = []
        page.gcode_loaded.connect(lambda path, data: emitted.append((path, data)))

        page.set_image("portrait.png", image)
        page.btn_convert.click()

        self.assertLess(len(emitted[-1][1].splitlines()), 1500)

    def test_settings_save_preserves_non_visual_config_fields(self) -> None:
        page = ConnectionPage()
        original = HostConfig(default_mode="WiFi", timeout_upload=91.0)
        emitted: list[HostConfig] = []
        page.save_requested.connect(emitted.append)
        page.load_config(original)

        page.btn_save.click()

        self.assertEqual(emitted[-1].default_mode, "WiFi")
        self.assertEqual(emitted[-1].timeout_upload, 91.0)

    def test_job_defaults_are_loaded_from_host_config(self) -> None:
        page = JobPage()
        config = HostConfig(job_id=17, timeout_cmd=33.5, preroll_bytes=2048, focus_power=7)

        page.load_config(config)

        self.assertEqual(page.job_id_edit.text(), "17")
        self.assertEqual(page.timeout_edit.text(), "33.5")
        self.assertEqual(page.preroll_edit.text(), "2048")
        self.assertEqual(page.focus_power.text(), "7")

    def test_job_page_only_exposes_firmware_backed_controls(self) -> None:
        page = JobPage()

        for removed in ("btn_pause", "btn_estop", "btn_estop_clear"):
            self.assertFalse(hasattr(page, removed))
        self.assertTrue(hasattr(page, "btn_resume"))
        self.assertTrue(hasattr(page, "btn_force_cancel"))

    def test_job_page_splits_manual_and_live_execution_controls(self) -> None:
        page = JobPage()

        group_titles = {group.title() for group in page.findChildren(QGroupBox)}
        self.assertIn("仅上传任务控制", group_titles)
        self.assertIn("上传并执行控制", group_titles)
        self.assertEqual(page.btn_exec.text(), "开始执行")
        self.assertEqual(page.btn_abort.text(), "清空已上传队列")
        self.assertEqual(page.btn_stop.text(), "暂停执行")
        self.assertEqual(page.btn_resume.text(), "恢复执行")
        self.assertEqual(page.btn_force_cancel.text(), "强制断光并取消任务")

    def test_logs_page_has_mockup_subtitle_and_console_header(self) -> None:
        page = LogsPage()

        subtitle = page.findChild(QLabel, "pageSubtitle")
        terminal_header = page.findChild(QLabel, "terminalHeader")
        self.assertIsNotNone(subtitle)
        self.assertIsNotNone(terminal_header)
        self.assertEqual(terminal_header.text(), "实时串口日志控制台")

    def test_logs_page_prunes_old_lines_with_pyside6_cursor_enums(self) -> None:
        page = LogsPage()
        page._line_count = 4000

        page._append("test line")

        self.assertEqual(page._line_count, 3501)

    def test_upload_captures_ui_values_before_worker_starts(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.page_gcode.editor.setPlainText("G21\nM30\n")
        window.page_job.job_id_edit.setText("23")
        window.page_job.timeout_edit.setText("12.5")
        window.page_job.preroll_edit.setText("1024")
        captured: list[object] = []

        def record_worker(function, *args) -> None:
            captured.extend((function, *args))

        window._run_job_worker = record_worker
        window._on_upload()

        self.assertEqual(captured[1:], [b"M30\n", 23, 12.5])

    def test_monitor_shows_upload_progress_but_not_execution_line_progress(self) -> None:
        page = JobPage()

        page.update_state(
            rx_state="数据传输中", rx_state_code=1, job_id=1,
            received=50, total=100, cache_free=1000,
            focus="OFF", tx_link="CONNECTED", rx_link="CONNECTED",
        )
        self.assertEqual(page.arc._value, 50)
        self.assertEqual(page.arc._caption, "下载进度")
        self.assertIn("50/100", page.lbl_substate.text())

        page.update_state(
            rx_state="正在执行", rx_state_code=3, job_id=1,
            received=100, total=100, cache_free=1000,
            focus="OFF", tx_link="CONNECTED", rx_link="CONNECTED",
        )
        self.assertEqual(page.arc._value, 0)
        self.assertEqual(page.arc._caption, "正在执行")
        self.assertEqual(page.lbl_substate.text(), "任务正在执行")
        self.assertNotIn("执行行数", page.cards)

    def test_status_parser_does_not_track_execution_line_progress(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)

        window._parse_rx_line(
            "@STATUS state=2 status=0 job=9 rx=1000 total=1000 free=500 lines=99"
        )
        self.assertEqual(window.state.rx_state_code, 2)

        window._parse_rx_line(
            "@STATUS state=3 status=0 job=9 rx=500 total=1000 free=500 lines=7"
        )

        self.assertEqual(window.state.rx_state_code, 3)
        self.assertEqual(window.page_job.arc._value, 0)
        self.assertEqual(window.page_job.arc._caption, "等待 RX 完成")
        self.assertTrue(window.page_job.arc._spinning)

    def test_status_is_parsed_regardless_of_log_channel(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)

        window._on_log_message(
            "tx",
            "@STATUS state=3 status=0 job=9 rx=1000 total=1000 free=500 lines=4",
        )

        self.assertEqual(window.state.rx_state_code, 3)
        self.assertEqual(window.page_job.arc._caption, "等待 RX 完成")
        self.assertTrue(window.page_job.arc._spinning)

    def test_status_transition_completes_without_rx_log_port(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)

        window._parse_rx_line(
            "@STATUS state=3 status=0 job=9 rx=1000 total=1000 free=500 lines=8"
        )
        window._parse_rx_line(
            "@STATUS state=0 status=0 job=9 rx=1000 total=1000 free=500 lines=10"
        )

        self.assertTrue(window.state.execution_complete)
        self.assertFalse(window.state.execution_expected)
        self.assertFalse(window.page_job.arc._spinning)
        self.assertEqual(window.page_job.arc._caption, "执行完成")
        self.assertIn("执行完成", window.page_job.lbl_task.text())

    def test_idle_status_completes_even_if_executing_status_was_missed(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.state.rx_state_code = 1
        window.state.rx_job_id = 9
        window.state.execution_expected = True
        window.page_job.set_execution_state(True)

        window._parse_rx_line(
            "@STATUS state=0 status=0 job=9 rx=1000 total=1000 free=500 lines=10"
        )

        self.assertTrue(window.state.execution_complete)
        self.assertFalse(window.page_job.arc._spinning)

    def test_log_view_failure_cannot_block_exec_done(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.state.rx_job_id = 9
        window.state.execution_expected = True
        window.page_job.set_execution_state(True)

        def fail_log_view(channel, message) -> None:
            raise AttributeError("log renderer failed")

        window.page_logs.append_log = fail_log_view
        window._on_log_message(
            "rx_log", "[JOB_EXEC] done job=9 lines=10 x_um=0 y_um=0"
        )

        self.assertTrue(window.state.execution_complete)
        self.assertFalse(window.page_job.arc._spinning)

    def test_late_log_after_file_close_is_ignored(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        log_file = io.StringIO()
        window._log_file = log_file
        log_file.close()

        window._on_log_message("host", "late log after close")

        self.assertIs(window._log_file, log_file)

    def test_stale_execution_status_cannot_restart_completed_spinner(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.state.rx_job_id = 9
        window._mark_execution_complete()

        window._on_task_status("执行中，等待 RX 完成", -1)

        self.assertTrue(window.state.execution_complete)
        self.assertFalse(window.page_job.arc._spinning)
        self.assertEqual(window.page_job.arc._caption, "执行完成")

    def test_late_executing_status_cannot_restart_completed_poll(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.state.tx_state = LinkState.CONNECTED
        window.client.is_open = lambda: True
        window.state.rx_job_id = 1
        window._mark_execution_complete()

        window._parse_rx_line(
            "@STATUS state=3 status=0 job=1 rx=184 total=184 free=102400 lines=13"
        )

        self.assertTrue(window.state.execution_complete)
        self.assertFalse(window.state.execution_expected)
        self.assertFalse(window._exec_poll_timer.isActive())
        self.assertFalse(window.page_job.arc._spinning)

    def test_focus_button_turns_red_when_focus_is_active(self) -> None:
        page = JobPage()

        page.set_focus_state(True)
        self.assertEqual(page.btn_focus.text(), "关闭调焦")
        self.assertTrue(page.btn_focus.property("active"))

        page.set_focus_state(False)
        self.assertEqual(page.btn_focus.text(), "开启调焦")
        self.assertFalse(page.btn_focus.property("active"))

    def test_execution_spinner_starts_and_stops_on_completion(self) -> None:
        page = JobPage()

        page.set_execution_state(True)
        self.assertTrue(page.arc._spinning)
        self.assertTrue(page.arc._spin_timer.isActive())

        page.set_execution_state(False, completed=True)
        self.assertFalse(page.arc._spinning)
        self.assertFalse(page.arc._spin_timer.isActive())
        self.assertEqual(page.arc._value, 100)
        self.assertEqual(page.arc._caption, "执行完成")

    def test_main_window_polls_status_only_during_execution(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)

        self.assertFalse(window._exec_poll_timer.isActive())

        window.state.tx_state = LinkState.CONNECTED
        window.client.is_open = lambda: True
        window._on_task_status("执行中，等待 RX 完成", -1)
        self.assertTrue(window._exec_poll_timer.isActive())

        window._mark_execution_complete()
        self.assertFalse(window._exec_poll_timer.isActive())

    def test_abort_return_to_idle_is_not_reported_as_success(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.state.rx_state_code = 3
        window.state.execution_expected = True
        window._on_task_status("已取消并清空", -1)

        window._parse_rx_line(
            "@STATUS state=0 status=0 job=9 rx=1000 total=1000 free=500 lines=10"
        )

        self.assertFalse(window.state.execution_complete)
        self.assertTrue(window.state.termination_requested)
        self.assertEqual(window.page_job.lbl_task.text(), "已取消并清空")

    def test_late_poll_error_after_abort_is_suppressed(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        logs: list[tuple[str, str]] = []
        window.worker.log.connect(lambda channel, message: logs.append((channel, message)))

        class FakeClient:
            def transact_status(self, timeout: float):
                raise TimeoutError("等待 @STATUS 超时")

            def close(self) -> None:
                pass

        window.client = FakeClient()
        window.state.execution_expected = True
        window.state.termination_requested = True
        window._exec_poll_timer.stop()

        window._execution_status_poll_worker()

        self.assertNotIn(("error", "[EXEC_POLL] 查询状态失败: 等待 @STATUS 超时"), logs)

    def test_exec_status_timeout_is_status_delay_not_error(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        logs: list[tuple[str, str]] = []
        timeouts: list[float] = []
        window.worker.log.connect(lambda channel, message: logs.append((channel, message)))

        class FakeClient:
            def transact_status(self, timeout: float):
                timeouts.append(timeout)
                raise TimeoutError("等待 @STATUS 超时")

            def close(self) -> None:
                pass

        window.client = FakeClient()
        window.state.execution_expected = True
        window._exec_poll_timer.start()

        window._execution_status_poll_worker()

        self.assertEqual(timeouts, [6.0])
        self.assertIn(("status", "[EXEC_POLL] 状态查询延迟: 等待 @STATUS 超时"), logs)
        self.assertNotIn(("error", "[EXEC_POLL] 查询状态失败: 等待 @STATUS 超时"), logs)

    def test_upload_only_explicitly_disables_preroll_execution(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        calls: list[tuple[tuple[object, ...], dict[str, object]]] = []

        class FakeClient:
            def upload_job(self, *args, **kwargs) -> None:
                calls.append((args, kwargs))

            def close(self) -> None:
                pass

        window.client = FakeClient()
        window._upload_worker(b"M5\n", 5, 20.0)

        self.assertEqual(len(calls), 1)
        self.assertNotIn("wait_ready", calls[0][1])
        self.assertEqual(calls[0][1]["preroll_bytes"], 0)
        self.assertFalse(calls[0][1]["start_on_preroll"])

    def test_upload_and_execute_enables_preroll_start_only_when_configured(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        uploads: list[dict[str, object]] = []
        controls: list[str] = []

        class FakeClient:
            def upload_job(self, *args, **kwargs) -> None:
                uploads.append(kwargs)

            def send_control(self, command, expect, timeout) -> None:
                controls.append(command)

            def close(self) -> None:
                pass

        window.client = FakeClient()
        large_job = (b"G1 X1\n" * 700) + b"S0\n" + (b"G1 X2\n" * 100)
        window._upload_exec_worker(large_job, 6, 20.0, 4096)
        self.assertEqual(uploads[-1]["preroll_bytes"], 4098)
        self.assertTrue(uploads[-1]["start_on_preroll"])
        self.assertNotIn("wait_ready", uploads[-1])
        self.assertEqual(controls, [])
        self.assertTrue(window.state.execution_expected)
        self.assertTrue(window.page_job.arc._spinning)

        window._upload_exec_worker(b"M5\n", 7, 20.0, 0)
        self.assertEqual(uploads[-1]["preroll_bytes"], 0)
        self.assertFalse(uploads[-1]["start_on_preroll"])
        self.assertEqual(controls, ["@EXEC_START 7"])

    def test_upload_execute_rejects_jobs_larger_than_cache_without_preroll(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        uploads: list[dict[str, object]] = []
        controls: list[str] = []

        class FakeClient:
            def upload_job(self, *args, **kwargs) -> None:
                uploads.append(kwargs)

            def send_control(self, command, expect, timeout) -> None:
                controls.append(command)

            def close(self) -> None:
                pass

        window.client = FakeClient()
        large_job = b"G1 X1\n" * ((JOB_MAX_SIZE // 6) + 2)
        self.assertGreater(len(large_job), JOB_MAX_SIZE)

        with self.assertRaisesRegex(RuntimeError, "超过 RX cache"):
            window._upload_exec_worker(large_job, 8, 20.0, 0)

        self.assertEqual(uploads, [])
        self.assertEqual(controls, [])

    def test_upload_execute_worker_releases_after_exec_start_ack(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)

        class FakeClient:
            def upload_job(self, *args, **kwargs) -> None:
                pass

            def send_control(self, command, expect, timeout) -> None:
                pass

            def close(self) -> None:
                pass

        window.client = FakeClient()
        window._run_job_worker(window._upload_exec_worker, b"M5\n", 3, 8.0, 0)

        self.assertTrue(window.worker.wait_for_idle(1000))
        self.app.processEvents()
        self.assertFalse(window.worker.is_busy())
        self.assertTrue(window.state.execution_expected)
        self.assertTrue(window.page_job.arc._spinning)

    def test_rx_monitor_updates_shared_link_state(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.rx_monitor.open = lambda port, baud: None

        window._on_rx_log("COM26", 115200)

        self.assertEqual(window.state.rx_state, LinkState.CONNECTED)
        self.assertEqual(window.page_conn.btn_rx_log.text(), "停止监听")
        window._on_stop_rx_log()
        self.assertEqual(window.state.rx_state, LinkState.DISCONNECTED)


if __name__ == "__main__":
    unittest.main()
