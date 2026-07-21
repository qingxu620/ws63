from __future__ import annotations

import io
import os
import time
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QPoint, QRect, QTimer, Qt
from PySide6.QtGui import QColor, QImage
from PySide6.QtTest import QTest
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QGroupBox,
    QLabel,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSpinBox,
    QTabWidget,
    QWidget,
)

from app.config_store import HostConfig
from app.state_models import AppState
from app.state_models import ConnectionMode, LinkState
from transports.sle_tx_transport import JOB_MAX_SIZE, JOB_PREROLL_BYTES
from ui.main_window import MainWindow
from ui.pages.connection_page import ConnectionPage
from ui.pages.gcode_page import GcodePage
from ui.pages.job_page import JobPage
from ui.pages.logs_page import LogsPage
from ui.widgets.crop_image_widget import CropImageWidget
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

    def test_job_page_exposes_exclusive_sle_wifi_mode_options(self) -> None:
        page = JobPage()
        requested: list[str] = []
        page.mode_requested.connect(requested.append)

        self.assertTrue(page.btn_sle.isChecked())
        page.btn_wifi.click()
        self.assertEqual(requested[-1], "WIFI")
        self.assertTrue(page.btn_wifi.isChecked())
        self.assertFalse(page.btn_sle.isChecked())

        page.btn_sle.click()
        self.assertEqual(requested[-1], "SLE")
        self.assertTrue(page.btn_sle.isChecked())
        self.assertFalse(page.btn_wifi.isChecked())

    def test_mode_options_send_explicit_mode_commands_through_main_window(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        workers: list[object] = []
        commands: list[tuple[str, str, float]] = []
        window.client.is_open = lambda: True
        window._run_job_worker = lambda fn, *args: workers.append(fn)
        window.client.send_control = (
            lambda command, expect, timeout: commands.append((command, expect, timeout))
        )

        window._on_mode_selected("WIFI")
        workers.pop()()
        window._on_mode_selected("SLE")
        workers.pop()()

        self.assertEqual(
            commands,
            [
                ("@MODE WIFI", "@OK mode_request target=WIFI", 8.0),
                ("@MODE SLE", "@OK mode_request target=SLE", 8.0),
            ],
        )

    def test_sle_active_event_confirms_mode_and_defers_status_while_busy(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        busy = [True]
        status_requests: list[bool] = []
        window.client.is_open = lambda: True
        window.worker.is_busy = lambda: busy[0]
        window._on_status = lambda: status_requests.append(True)

        window._process_protocol_message("@LINK peer=RX state=CONNECTED")
        window._process_protocol_message("@LINK peer=SCREEN state=CONNECTING")
        window._process_protocol_message(
            "@MODE active=SLE rx=ready screen=connecting"
        )

        self.assertEqual(window.state.mode, ConnectionMode.SLE_VIA_TX)
        self.assertEqual(window.state.mode_transition, "ACTIVE")
        self.assertEqual(window.state.sle_rx_state, LinkState.CONNECTED)
        self.assertEqual(window.state.screen_state, LinkState.CONNECTING)
        self.assertTrue(window.page_job.btn_sle.isChecked())
        self.assertTrue(window._mode_status_sync_pending)
        self.assertFalse(status_requests)

        busy[0] = False
        window._try_mode_status_sync()

        self.assertFalse(window._mode_status_sync_pending)
        self.assertEqual(status_requests, [True])

    def test_wifi_mode_event_remains_pending_until_external_verification(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)

        window._process_protocol_message(
            "@MODE target=WIFI state=PENDING reason=external_verify"
        )

        self.assertEqual(window.state.mode, ConnectionMode.SLE_VIA_TX)
        self.assertEqual(window.state.mode_target, ConnectionMode.WIFI_TCP)
        self.assertEqual(window.state.mode_transition, "PENDING")
        self.assertTrue(window.page_job.btn_wifi.isChecked())
        self.assertFalse(window.page_job.btn_upload_exec.isEnabled())
        self.assertFalse(window.page_job.btn_focus.isEnabled())
        self.assertIn("外部验证", window.page_job.lbl_mode_status.text())

        window._process_protocol_message(
            "@MODE target=WIFI state=FAILED reason=rx_busy"
        )
        self.assertEqual(window.state.mode_transition, "FAILED")
        self.assertIn("rx_busy", window.page_job.lbl_mode_status.text())

        window._process_protocol_message("@MODE active=SLE rx=ready screen=ready")
        self.assertTrue(window.page_job.btn_upload_exec.isEnabled())
        self.assertTrue(window.page_job.btn_focus.isEnabled())

    def test_gcode_page_contains_ai_and_editor_tabs(self) -> None:
        page = GcodePage()

        tabs = page.findChild(QTabWidget, "gcodeTabs")
        self.assertIsNotNone(tabs)
        self.assertEqual(tabs.count(), 2)
        self.assertEqual(tabs.tabText(0), "AI 生图与矢量化")
        self.assertEqual(tabs.tabText(1), "G-code 编辑器")
        self.assertFalse(page.btn_adjust_gcode.isEnabled())

        page.set_content("finished.gcode", b"G90\nG0 X1 Y1\nM5\n")

        self.assertTrue(page.btn_adjust_gcode.isEnabled())

    def test_finished_gcode_load_builds_actual_trajectory_preview_in_background(self) -> None:
        page = GcodePage()
        self.addCleanup(page.close)
        source = (
            b"G21\nG90\nM4 S0\nG0 X10 Y20\nS250\n"
            b"G1 X30 Y20\nG1 X30 Y40 S500\nS0\nM5\n"
        )

        started = time.perf_counter()
        page.set_content("finished.gcode", source, render_preview=True)
        start_elapsed = time.perf_counter() - started
        deadline = time.perf_counter() + 5.0
        while page._gcode_preview_task is not None and time.perf_counter() < deadline:
            self.app.processEvents()
            time.sleep(0.002)

        self.assertLess(start_elapsed, 0.1)
        self.assertIsNone(page._gcode_preview_task)
        self.assertTrue(page._converted_image.isNull() is False)
        self.assertEqual(page.btn_preview_original.text(), "无原图")
        self.assertEqual(page.btn_preview_converted.text(), "G-code预览")
        self.assertTrue(page.btn_preview_converted.isEnabled())
        self.assertTrue(page.btn_preview_converted.isChecked())
        self.assertIn("2 条开光 G1 线段", page.image_status.text())
        self.assertIn("20.00×20.00 mm", page.image_status.text())

    def test_finished_gcode_without_laser_path_explains_why_preview_is_empty(self) -> None:
        page = GcodePage()
        self.addCleanup(page.close)

        page.set_content(
            "travel_only.gcode",
            b"G90\nG0 X1 Y1\nG1 X2 Y2 S500\nM5\n",
            render_preview=True,
        )
        deadline = time.perf_counter() + 5.0
        while page._gcode_preview_task is not None and time.perf_counter() < deadline:
            self.app.processEvents()
            time.sleep(0.002)

        self.assertIsNone(page._gcode_preview_task)
        self.assertTrue(page._converted_image.isNull())
        self.assertIn("没有可显示的打标图像", page.preview_label.text())
        self.assertIn("0 条有效打标轨迹", page.image_status.text())

    def test_finished_gcode_adjust_dialog_applies_all_parameters(self) -> None:
        page = GcodePage()
        self.addCleanup(page.close)
        page.set_content(
            "finished.gcode",
            b"G90\nM4 S0\nF1000\nG0 X10 Y20\nS250\nG1 X30 Y40 S500\nM5\n",
        )

        def apply_dialog(dialog: QDialog) -> int:
            self.assertEqual(dialog.windowTitle(), "成品 G-code 参数调整")
            power = dialog.findChild(QSpinBox, "gcodeAdjustMaxPower")
            feed = dialog.findChild(QSpinBox, "gcodeAdjustMaxFeed")
            scale = dialog.findChild(QDoubleSpinBox, "gcodeAdjustScalePercent")
            offset_x = dialog.findChild(QDoubleSpinBox, "gcodeAdjustOffsetX")
            offset_y = dialog.findChild(QDoubleSpinBox, "gcodeAdjustOffsetY")
            summary = dialog.findChild(QLabel, "gcodeAdjustResultSummary")
            buttons = dialog.findChild(QDialogButtonBox)
            assert power is not None
            assert feed is not None
            assert scale is not None
            assert offset_x is not None
            assert offset_y is not None
            assert summary is not None
            assert buttons is not None

            power.setValue(800)
            feed.setValue(2000)
            scale.setValue(200.0)
            offset_x.setValue(5.0)
            offset_y.setValue(-5.0)
            self.assertIn("X 15.000-55.000", summary.text())
            self.assertIn("范围有效", summary.text())
            buttons.button(QDialogButtonBox.StandardButton.Ok).click()
            return dialog.result()

        with patch.object(QDialog, "exec", apply_dialog):
            page.btn_adjust_gcode.click()
            deadline = time.perf_counter() + 5.0
            while (
                page.get_path() != "finished.adjusted.gcode"
                and time.perf_counter() < deadline
            ):
                self.app.processEvents()
                time.sleep(0.002)
            while (
                page._gcode_preview_task is not None
                and time.perf_counter() < deadline
            ):
                self.app.processEvents()
                time.sleep(0.002)

        adjusted = page.get_gcode_bytes().decode("utf-8")
        self.assertIsNone(page._gcode_adjust_task)
        self.assertEqual(page.get_path(), "finished.adjusted.gcode")
        self.assertIn("F2000", adjusted)
        self.assertIn("S400", adjusted)
        self.assertIn("S800", adjusted)
        self.assertIn("X15 Y15", adjusted)
        self.assertIn("X55 Y55", adjusted)
        self.assertIn("已调整", page.lbl_file.text())
        self.assertIn("G-code 实际打标轨迹", page.image_status.text())

    def test_large_gcode_adjustment_starts_in_background_and_can_cancel(self) -> None:
        page = GcodePage()
        self.addCleanup(page.close)
        source = "G90\nM4 S0\nF3000\n" + ("G1 X10 Y10 S500\n" * 120_000) + "M5\n"
        original = source.encode("utf-8")
        page.set_content("large_finished.gcode", original)

        started = time.perf_counter()
        page.btn_adjust_gcode.click()
        start_elapsed = time.perf_counter() - started

        self.assertLess(start_elapsed, 0.1)
        self.assertIsNotNone(page._gcode_adjust_task)
        self.assertEqual(page.btn_adjust_gcode.text(), "取消调整")
        self.assertIn("正在分析 G-code", page.lbl_file.text())
        self.assertTrue(page.editor.isReadOnly())

        page.btn_adjust_gcode.click()
        deadline = time.perf_counter() + 5.0
        while page._gcode_adjust_tasks and time.perf_counter() < deadline:
            self.app.processEvents()
            time.sleep(0.002)

        self.assertIsNone(page._gcode_adjust_task)
        self.assertFalse(page._gcode_adjust_tasks)
        self.assertEqual(page.btn_adjust_gcode.text(), "调整参数…")
        self.assertEqual(page.get_gcode_bytes(), original)

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

    def test_scanline_quality_controls_generated_row_count(self) -> None:
        page = GcodePage()
        image = QImage(10, 5, QImage.Format.Format_RGB32)
        image.fill(QColor("black"))
        page.size_spin.setValue(20.0)
        page.target_height_spin.setValue(20.0)
        page.set_image("quality.png", image)

        page.scan_quality_spin.setValue(5.0)
        page.btn_convert.click()
        low_quality_rows = sum(
            line.startswith("G0 ") for line in page.editor.toPlainText().splitlines()
        )

        page.scan_quality_spin.setValue(10.0)
        page.btn_convert.click()
        high_quality_rows = sum(
            line.startswith("G0 ") for line in page.editor.toPlainText().splitlines()
        )

        self.assertEqual(low_quality_rows, 50)
        self.assertEqual(high_quality_rows, 100)
        self.assertIn("10.0 线/mm", page.image_status.text())

    def test_manual_crop_changes_conversion_source_and_can_restore(self) -> None:
        page = GcodePage()
        image = QImage(10, 8, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        page.set_image("crop.png", image)

        page._apply_crop(QRect(2, 1, 5, 4))

        self.assertEqual((page._source_image.width(), page._source_image.height()), (5, 4))
        self.assertIn("裁剪已生效", page.image_status.text())

        page._restore_original_image()

        self.assertEqual((page._source_image.width(), page._source_image.height()), (10, 8))

    def test_locked_target_size_tracks_cropped_aspect_in_both_directions(self) -> None:
        page = GcodePage()
        image = QImage(100, 50, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        page.set_image("locked_crop.png", image)

        self.assertAlmostEqual(page.size_spin.value(), 99.0, places=1)
        self.assertAlmostEqual(page.target_height_spin.value(), 49.5, places=1)

        page._apply_crop(QRect(0, 0, 80, 20))
        self.assertAlmostEqual(page.size_spin.value(), 99.0, places=1)
        self.assertAlmostEqual(page.target_height_spin.value(), 24.8, places=1)

        page.size_spin.setValue(80.0)
        self.assertAlmostEqual(page.target_height_spin.value(), 20.0, places=1)
        page.target_height_spin.setValue(10.0)
        self.assertAlmostEqual(page.size_spin.value(), 40.0, places=1)

    def test_target_dialog_auto_size_updates_immediately_from_dpi_and_offset(self) -> None:
        page = GcodePage()
        image = QImage(1000, 200, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        page.set_image("auto_target.png", image)
        page.auto_size_check.setChecked(True)
        page.dpi_spin.setValue(254)

        def inspect_dialog(dialog: QDialog) -> int:
            width = dialog.findChild(QDoubleSpinBox, "dialogMarkWidthMm")
            height = dialog.findChild(QDoubleSpinBox, "dialogMarkHeightMm")
            dpi = dialog.findChild(QSpinBox, "dialogTargetDpi")
            auto = dialog.findChild(QCheckBox, "dialogAutoTargetSize")
            offset_x = dialog.findChild(QDoubleSpinBox, "dialogOffsetX")
            ratio = dialog.findChild(QLabel, "dialogSourceAspectRatio")
            buttons = dialog.findChild(QDialogButtonBox)
            assert width is not None
            assert height is not None
            assert dpi is not None
            assert auto is not None
            assert offset_x is not None
            assert ratio is not None
            assert buttons is not None

            self.assertTrue(auto.isChecked())
            self.assertFalse(width.isEnabled())
            self.assertFalse(height.isEnabled())
            self.assertAlmostEqual(width.value(), 99.0, places=1)
            self.assertAlmostEqual(height.value(), 19.8, places=1)
            self.assertIn("1000×200", ratio.text())

            dpi.setValue(508)
            self.assertAlmostEqual(width.value(), 50.0, places=1)
            self.assertAlmostEqual(height.value(), 10.0, places=1)

            offset_x.setValue(60.0)
            self.assertAlmostEqual(width.value(), 39.0, places=1)
            self.assertAlmostEqual(height.value(), 7.8, places=1)
            buttons.button(QDialogButtonBox.StandardButton.Ok).click()
            return dialog.result()

        with patch.object(QDialog, "exec", inspect_dialog):
            page._open_target_parameter_dialog()

        self.assertTrue(page.auto_size_check.isChecked())
        self.assertAlmostEqual(page.size_spin.value(), 39.0, places=1)
        self.assertAlmostEqual(page.target_height_spin.value(), 7.8, places=1)

    def test_target_dialog_lock_uses_current_cropped_ratio(self) -> None:
        page = GcodePage()
        image = QImage(500, 100, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        page.set_image("dialog_lock.png", image)
        page.auto_size_check.setChecked(False)

        def inspect_dialog(dialog: QDialog) -> int:
            width = dialog.findChild(QDoubleSpinBox, "dialogMarkWidthMm")
            height = dialog.findChild(QDoubleSpinBox, "dialogMarkHeightMm")
            offset_x = dialog.findChild(QDoubleSpinBox, "dialogOffsetX")
            offset_y = dialog.findChild(QDoubleSpinBox, "dialogOffsetY")
            dpi = dialog.findChild(QSpinBox, "dialogTargetDpi")
            speed = dialog.findChild(QSpinBox, "dialogBoundarySpeedMmMin")
            fill_speed = dialog.findChild(QSpinBox, "dialogFillSpeedMmMin")
            min_s = dialog.findChild(QSpinBox, "dialogMinimumPowerS")
            max_s = dialog.findChild(QSpinBox, "dialogMaximumPowerS")
            lock = dialog.findChild(QCheckBox, "dialogLockAspect")
            size_row = dialog.findChild(QWidget, "dialogMarkSizeRow")
            offset_row = dialog.findChild(QWidget, "dialogOffsetRow")
            buttons = dialog.findChild(QDialogButtonBox)
            assert width is not None
            assert height is not None
            assert offset_x is not None
            assert offset_y is not None
            assert dpi is not None
            assert speed is not None
            assert fill_speed is not None
            assert min_s is not None
            assert max_s is not None
            assert lock is not None
            assert size_row is not None
            assert offset_row is not None
            assert buttons is not None
            for spin in (dpi, width, height, offset_x, offset_y):
                self.assertGreaterEqual(spin.minimumWidth(), 130)
                self.assertEqual(
                    spin.sizePolicy().horizontalPolicy(),
                    QSizePolicy.Policy.Expanding,
                )
                self.assertGreater(spin.maximumWidth(), spin.minimumWidth())
            for spin in (
                speed,
                fill_speed,
                min_s,
                max_s,
                dpi,
                width,
                height,
                offset_x,
                offset_y,
            ):
                self.assertFalse(spin.keyboardTracking())
                self.assertTrue(spin.isAccelerated())
            self.assertEqual(speed.singleStep(), 100)
            self.assertEqual(fill_speed.singleStep(), 100)
            self.assertEqual(min_s.singleStep(), 10)
            self.assertEqual(max_s.singleStep(), 10)
            self.assertEqual(dpi.singleStep(), 10)
            self.assertAlmostEqual(width.singleStep(), 0.1)
            self.assertAlmostEqual(height.singleStep(), 0.1)
            self.assertAlmostEqual(offset_x.singleStep(), 0.1)
            self.assertAlmostEqual(offset_y.singleStep(), 0.1)
            self.assertEqual(size_row.layout().stretch(0), 1)
            self.assertEqual(size_row.layout().stretch(1), 1)
            self.assertEqual(offset_row.layout().stretch(0), 1)
            self.assertEqual(offset_row.layout().stretch(1), 1)
            lock.setChecked(True)
            width_events: list[float] = []
            width.valueChanged.connect(width_events.append)
            width.lineEdit().setFocus()
            width.lineEdit().selectAll()
            QTest.keyClicks(width.lineEdit(), "71.5")
            self.assertEqual(width.lineEdit().text(), "W: 71.5")
            self.assertEqual(width_events, [])
            QTest.keyClick(width.lineEdit(), Qt.Key.Key_Return)
            self.assertEqual(width_events, [71.5])
            self.assertAlmostEqual(width.value(), 71.5, places=1)
            self.assertAlmostEqual(height.value(), 14.3, places=1)
            height.setValue(10.0)
            self.assertAlmostEqual(width.value(), 50.0, places=1)
            width.lineEdit().setFocus()
            width.lineEdit().selectAll()
            QTest.keyClicks(width.lineEdit(), "72.5")
            self.assertAlmostEqual(width.value(), 50.0, places=1)
            buttons.button(QDialogButtonBox.StandardButton.Ok).click()
            return dialog.result()

        with patch.object(QDialog, "exec", inspect_dialog):
            page._open_target_parameter_dialog()

        self.assertAlmostEqual(page.size_spin.value(), 72.5, places=1)
        self.assertAlmostEqual(page.target_height_spin.value(), 14.5, places=1)

    def test_image_parameter_dialog_contains_quality_and_crop_controls(self) -> None:
        page = GcodePage()
        image = QImage(10, 8, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        page.set_image("dialog.png", image)
        dialogs: list[QDialog] = []

        with patch.object(
            QDialog,
            "exec",
            lambda dialog: dialogs.append(dialog) or 0,
        ):
            page._open_image_parameter_dialog()

        self.assertEqual(len(dialogs), 1)
        dialog = dialogs[0]
        quality = dialog.findChild(
            QDoubleSpinBox,
            "dialogScanQualityLinesPerMm",
        )
        crop = dialog.findChild(QWidget, "dialogCropImage")
        auto_crop = dialog.findChild(QPushButton, "dialogAutoCrop")
        crop_actions = dialog.findChild(QWidget, "dialogCropActions")
        crop_tip = dialog.findChild(QLabel, "dialogCropTip")
        geometry_actions = dialog.findChild(QWidget, "dialogGeometryActions")
        geometry_tip = dialog.findChild(QLabel, "dialogGeometryTip")
        auto_line_art = dialog.findChild(QCheckBox, "dialogAutoLineArtCompression")
        restore = dialog.findChild(QPushButton, "dialogRestoreOriginal")
        self.assertIsNotNone(quality)
        self.assertIsNotNone(crop)
        self.assertIsNotNone(auto_crop)
        self.assertIsNotNone(crop_actions)
        self.assertIsNotNone(crop_tip)
        self.assertIsNotNone(geometry_actions)
        self.assertIsNotNone(geometry_tip)
        self.assertIsNotNone(auto_line_art)
        self.assertIsNotNone(restore)
        self.assertIs(auto_crop.parentWidget(), crop_actions)
        self.assertIsNot(crop_tip.parentWidget(), crop_actions)
        for button in crop_actions.findChildren(QPushButton):
            required_width = (
                button.fontMetrics().horizontalAdvance(button.text()) + 40
            )
            self.assertGreaterEqual(button.minimumWidth(), required_width)
        for button in geometry_actions.findChildren(QPushButton):
            required_width = (
                button.fontMetrics().horizontalAdvance(button.text()) + 40
            )
            self.assertGreaterEqual(button.minimumWidth(), required_width)
        self.assertIsNone(page.findChild(QPushButton, "btnCropImage"))

    def test_scan_quality_updates_trajectory_without_rebuilding_preview(self) -> None:
        page = GcodePage()
        image = QImage(2048, 2048, QImage.Format.Format_RGB32)
        image.fill(QColor("#64748b"))
        page.set_image("quality_responsiveness.png", image)

        def inspect_dialog(dialog: QDialog) -> int:
            quality = dialog.findChild(
                QDoubleSpinBox,
                "dialogScanQualityLinesPerMm",
            )
            threshold = dialog.findChild(QSpinBox, "dialogThresholdValue")
            debounce = dialog.findChild(QTimer, "dialogPreviewDebounce")
            assert quality is not None
            assert threshold is not None
            assert debounce is not None
            debounce.stop()

            self.assertFalse(quality.keyboardTracking())
            self.assertTrue(quality.isAccelerated())
            self.assertFalse(threshold.keyboardTracking())
            self.assertTrue(threshold.isAccelerated())

            for value in (11.0, 12.0, 20.0, 50.0):
                quality.setValue(value)
            self.assertFalse(debounce.isActive())

            threshold.setValue(threshold.value() + 1)
            self.assertTrue(debounce.isActive())
            return 0

        with patch.object(QDialog, "exec", inspect_dialog):
            page._open_image_parameter_dialog()

    def test_ai_generation_button_becomes_a_cancel_action(self) -> None:
        page = GcodePage()
        generation_requests: list[tuple[str, str, bool, str]] = []
        cancel_requests: list[bool] = []
        page.ai_generation_requested.connect(
            lambda *request: generation_requests.append(request)
        )
        page.ai_generation_cancel_requested.connect(
            lambda: cancel_requests.append(True)
        )

        page.update_ui_generating_state(True)

        self.assertTrue(page.btn_generate.isEnabled())
        self.assertEqual(page.btn_generate.text(), "取消生成")
        self.assertEqual(page.btn_generate.objectName(), "btnDanger")
        page.btn_generate.click()
        self.assertEqual(cancel_requests, [True])
        self.assertEqual(generation_requests, [])
        self.assertEqual(page.btn_generate.text(), "正在取消...")

        page.update_ui_generating_state(False)
        self.assertTrue(page.btn_generate.isEnabled())
        self.assertEqual(page.btn_generate.text(), "生成图像")
        self.assertEqual(page.btn_generate.objectName(), "btnPrimary")

    def test_abandoned_ai_result_cannot_replace_current_preview(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        abandoned_worker = object()
        current_worker = object()
        window._doubao_worker = current_worker

        window._on_ai_image_ready(abandoned_worker, "late-result.png")

        self.assertTrue(window.page_gcode._source_image.isNull())
        self.assertIs(window._doubao_worker, current_worker)
        window._doubao_worker = None

    def test_image_parameter_dialog_applies_crop_and_quality_together(self) -> None:
        page = GcodePage()
        image = QImage(10, 8, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        page.set_image("dialog_apply.png", image)

        def apply_dialog(dialog: QDialog) -> int:
            crop = dialog.findChild(CropImageWidget, "dialogCropImage")
            crop_status = dialog.findChild(QLabel, "dialogCropStatus")
            preview_tabs = dialog.findChild(QTabWidget, "lasergrblPreviewTabs")
            quality = dialog.findChild(
                QDoubleSpinBox,
                "dialogScanQualityLinesPerMm",
            )
            buttons = dialog.findChild(QDialogButtonBox)
            assert crop is not None
            assert crop_status is not None
            assert preview_tabs is not None
            assert quality is not None
            assert buttons is not None
            crop.set_selection_rect(QRect(2, 1, 5, 4))
            self.assertIn("待应用裁切", crop_status.text())
            self.assertIn("10×8", crop_status.text())
            self.assertIn("5×4", crop_status.text())
            self.assertIn("待应用", preview_tabs.tabText(0))
            self.assertEqual(
                buttons.button(QDialogButtonBox.StandardButton.Ok).text(),
                "应用参数与裁切",
            )
            quality.setValue(18.0)
            buttons.button(QDialogButtonBox.StandardButton.Ok).click()
            return dialog.result()

        with patch.object(QDialog, "exec", apply_dialog):
            page._open_image_parameter_dialog()

        self.assertEqual((page._source_image.width(), page._source_image.height()), (5, 4))
        self.assertEqual(page.scan_quality_spin.value(), 18.0)
        self.assertIn("裁剪已生效", page.image_status.text())
        self.assertEqual(page.btn_preview_original.text(), "裁切后")

    def test_image_parameter_dialog_exposes_lasergrbl_preprocess_controls(self) -> None:
        page = GcodePage()
        image = QImage(4, 3, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        page.size_spin.setValue(4.0)
        page.target_height_spin.setValue(3.0)
        page.set_image("lasergrbl_controls.png", image)
        dialogs: list[QDialog] = []

        with patch.object(
            QDialog,
            "exec",
            lambda dialog: dialogs.append(dialog) or 0,
        ):
            page._open_image_parameter_dialog()

        self.assertEqual(len(dialogs), 1)
        dialog = dialogs[0]
        parameter_scroll = dialog.findChild(
            QScrollArea,
            "laserDialogParameterScroll",
        )
        preview_tabs = dialog.findChild(QTabWidget, "lasergrblPreviewTabs")
        formula = dialog.findChild(QComboBox, "dialogGrayscaleFormula")
        dither = dialog.findChild(QComboBox, "dialogDitherAlgorithm")

        self.assertIsNotNone(parameter_scroll)
        assert parameter_scroll is not None
        self.assertEqual(
            parameter_scroll.horizontalScrollBarPolicy(),
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff,
        )
        self.assertIsNotNone(preview_tabs)
        assert preview_tabs is not None
        self.assertEqual(preview_tabs.count(), 2)
        self.assertEqual(
            [formula.itemData(index) for index in range(formula.count())],
            ["optical_correct", "simple_average", "weight_average", "custom"],
        )
        self.assertEqual(
            [dither.itemData(index) for index in range(dither.count())],
            [
                "floyd_steinberg",
                "atkinson",
                "burks",
                "jarvis",
                "random",
                "sierra2",
                "sierra3",
                "sierra_lite",
                "stucki",
            ],
        )
        for object_name in (
            "dialogInvertImage",
            "dialogThresholdEnabled",
            "dialogRasterPwmEnabled",
            "dialogUnidirectionalScan",
            "dialogRapidWhiteTravel",
        ):
            self.assertIsNotNone(dialog.findChild(QCheckBox, object_name))
        for object_name in (
            "dialogRotateLeft",
            "dialogRotateRight",
            "dialogFlipHorizontal",
            "dialogFlipVertical",
            "dialogInvertColors",
        ):
            self.assertIsNotNone(dialog.findChild(QPushButton, object_name))
        self.assertIsNotNone(
            dialog.findChild(QComboBox, "dialogScanDirection")
        )
        self.assertIsNotNone(
            dialog.findChild(QDoubleSpinBox, "dialogVectorQualityPixelsPerMm")
        )
        self.assertIsNotNone(
            dialog.findChild(
                QDoubleSpinBox,
                "dialogVectorFillQualityLinesPerMm",
            )
        )
        scan_quality = dialog.findChild(
            QDoubleSpinBox,
            "dialogScanQualityLinesPerMm",
        )
        self.assertIsNotNone(scan_quality)
        assert scan_quality is not None
        self.assertEqual(scan_quality.maximum(), 50.0)

    def test_image_parameter_dialog_applies_lasergrbl_options(self) -> None:
        page = GcodePage()
        image = QImage(4, 3, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        page.size_spin.setValue(4.0)
        page.target_height_spin.setValue(3.0)
        page.set_image("lasergrbl_options.png", image)

        def apply_dialog(dialog: QDialog) -> int:
            formula = dialog.findChild(QComboBox, "dialogGrayscaleFormula")
            invert = dialog.findChild(QCheckBox, "dialogInvertImage")
            threshold = dialog.findChild(QCheckBox, "dialogThresholdEnabled")
            pwm = dialog.findChild(QCheckBox, "dialogRasterPwmEnabled")
            direction = dialog.findChild(QComboBox, "dialogScanDirection")
            unidirectional = dialog.findChild(
                QCheckBox,
                "dialogUnidirectionalScan",
            )
            rapid_white = dialog.findChild(
                QCheckBox,
                "dialogRapidWhiteTravel",
            )
            dither = dialog.findChild(QComboBox, "dialogDitherAlgorithm")
            vector_quality = dialog.findChild(
                QDoubleSpinBox,
                "dialogVectorQualityPixelsPerMm",
            )
            fill_quality = dialog.findChild(
                QDoubleSpinBox,
                "dialogVectorFillQualityLinesPerMm",
            )
            buttons = dialog.findChild(QDialogButtonBox)
            assert formula is not None
            assert invert is not None
            assert threshold is not None
            assert pwm is not None
            assert direction is not None
            assert unidirectional is not None
            assert rapid_white is not None
            assert dither is not None
            assert vector_quality is not None
            assert fill_quality is not None
            assert buttons is not None

            formula.setCurrentIndex(formula.findData("custom"))
            invert.setChecked(True)
            threshold.setChecked(True)
            pwm.setChecked(False)
            direction.setCurrentIndex(direction.findData("vertical"))
            unidirectional.setChecked(True)
            rapid_white.setChecked(False)
            dither.setCurrentIndex(dither.findData("atkinson"))
            vector_quality.setValue(18.0)
            fill_quality.setValue(7.0)
            buttons.button(QDialogButtonBox.StandardButton.Ok).click()
            return dialog.result()

        with patch.object(QDialog, "exec", apply_dialog):
            page._open_image_parameter_dialog()

        self.assertEqual(page.grayscale_formula_combo.currentData(), "custom")
        self.assertTrue(page.invert_image_check.isChecked())
        self.assertTrue(page.threshold_enabled_check.isChecked())
        self.assertFalse(page.raster_pwm_check.isChecked())
        self.assertEqual(page.scan_direction_combo.currentData(), "vertical")
        self.assertTrue(page.unidirectional_scan_check.isChecked())
        self.assertFalse(page.rapid_white_check.isChecked())
        self.assertEqual(page.dither_algorithm_combo.currentData(), "atkinson")
        self.assertEqual(page.vector_quality_spin.value(), 18.0)
        self.assertEqual(page.fill_quality_spin.value(), 7.0)

    def test_image_conversion_has_no_upload_only_size_warning(self) -> None:
        page = GcodePage()
        image = QImage(4, 4, QImage.Format.Format_RGB32)
        image.fill(QColor("black"))
        page.set_image("large_generated.png", image)
        large_gcode = (
            "M4 S0\nF3000\n"
            + ("G0 X1 Y1\nS1000\nG1 X2 Y2\nS0\n" * 4000)
            + "M5\n"
        )
        self.assertGreater(len(large_gcode.encode("utf-8")), JOB_MAX_SIZE)

        with patch(
            "ui.pages.gcode_page.grayscale_rows_to_gcode",
            return_value=large_gcode,
        ):
            page.btn_convert.click()

        self.assertGreater(len(page.get_gcode_bytes()), JOB_MAX_SIZE)
        self.assertNotIn("上传上限", page.image_status.text())
        self.assertNotIn("100KiB", page.image_status.text())

    def test_black_white_line_art_is_automatically_compacted(self) -> None:
        image = QImage(300, 60, QImage.Format.Format_RGB32)
        for y in range(image.height()):
            for x in range(image.width()):
                gray = 248 + ((x * 5 + y * 3) % 8)
                if 70 <= x < 230 and 15 <= y < 45:
                    gray = (x + y) % 8
                image.setPixelColor(x, y, QColor(gray, gray, gray))

        compact = GcodePage()
        compact.auto_size_check.setChecked(False)
        compact.size_spin.setValue(99.0)
        compact.target_height_spin.setValue(20.0)
        compact.scan_quality_spin.setValue(18.0)
        compact.set_image("line_art.png", image)
        compact.btn_convert.click()

        full_pwm = GcodePage()
        full_pwm.auto_size_check.setChecked(False)
        full_pwm.size_spin.setValue(99.0)
        full_pwm.target_height_spin.setValue(20.0)
        full_pwm.scan_quality_spin.setValue(18.0)
        full_pwm.auto_line_art_check.setChecked(False)
        full_pwm.set_image("line_art.png", image)
        full_pwm.btn_convert.click()

        self.assertIn("自动识别黑白线稿", compact.image_status.text())
        self.assertLess(len(compact.get_gcode_bytes()), len(full_pwm.get_gcode_bytes()) // 4)

    def test_visible_raster_conversion_runs_without_blocking_ui(self) -> None:
        page = GcodePage()
        self.addCleanup(page.close)
        image = QImage(240, 60, QImage.Format.Format_RGB32)
        for y in range(image.height()):
            for x in range(image.width()):
                gray = (x * 17 + y * 29) % 256
                image.setPixelColor(x, y, QColor(gray, gray, gray))
        page.auto_size_check.setChecked(False)
        page.size_spin.setValue(40.0)
        page.target_height_spin.setValue(10.0)
        page.scan_quality_spin.setValue(12.0)
        page.set_image("responsive.png", image)
        page.show()
        self.app.processEvents()

        ticks = [0]
        timer = QTimer()
        timer.setInterval(5)
        timer.timeout.connect(lambda: ticks.__setitem__(0, ticks[0] + 1))
        timer.start()
        started = time.perf_counter()
        page.btn_convert.click()
        click_elapsed = time.perf_counter() - started
        deadline = time.perf_counter() + 20.0
        while page._conversion_task is not None and time.perf_counter() < deadline:
            self.app.processEvents()
            time.sleep(0.002)
        timer.stop()

        self.assertLess(click_elapsed, 0.1)
        self.assertIsNone(page._conversion_task)
        self.assertGreater(ticks[0], 3)
        self.assertTrue(page.get_gcode_bytes())
        self.assertEqual(page.btn_convert.text(), "转换为 G-code")

    def test_large_gcode_keeps_full_payload_without_blocking_editor_load(self) -> None:
        page = GcodePage()
        large_gcode = b"M4 S0\n" + (b"G1 X1.000 Y1.000 S500\n" * 100_000) + b"S0\nM5\n"

        page.set_content("large.generated.gcode", large_gcode)

        visible_bytes = page.editor.toPlainText().encode("utf-8")
        self.assertLess(len(visible_bytes), len(large_gcode))
        self.assertTrue(page.editor.isReadOnly())
        self.assertFalse(page.btn_load_full_gcode.isHidden())
        self.assertEqual(page.get_gcode_bytes(), large_gcode)
        self.assertIn("传输使用完整内容", page.lbl_size.text())

    def test_conversion_result_load_failure_never_leaves_busy_state(self) -> None:
        page = GcodePage()
        self.addCleanup(page.close)
        image = QImage(16, 4, QImage.Format.Format_RGB32)
        image.fill(QColor("black"))
        page.auto_size_check.setChecked(False)
        page.size_spin.setValue(16.0)
        page.target_height_spin.setValue(4.0)
        page.scan_quality_spin.setValue(1.0)
        page.set_image("result_failure.png", image)
        page.show()
        self.app.processEvents()

        with patch.object(page, "set_content", side_effect=RuntimeError("editor load failed")):
            page.btn_convert.click()
            deadline = time.perf_counter() + 5.0
            while page._conversion_task is not None and time.perf_counter() < deadline:
                self.app.processEvents()
                time.sleep(0.002)

        self.assertIsNone(page._conversion_task)
        self.assertEqual(page.btn_convert.text(), "转换为 G-code")
        self.assertTrue(page.btn_image_params.isEnabled())
        self.assertIn("转换结果装载失败", page.image_status.text())

    def test_scanline_grayscale_pwm_emits_multiple_power_levels(self) -> None:
        page = GcodePage()
        image = QImage(5, 1, QImage.Format.Format_RGB32)
        for x, gray in enumerate((0, 64, 128, 192, 255)):
            image.setPixelColor(x, 0, QColor(gray, gray, gray))
        page.resize_mode_combo.setCurrentIndex(
            page.resize_mode_combo.findData("nearest")
        )
        page.size_spin.setValue(5.0)
        page.target_height_spin.setValue(1.0)
        page.scan_quality_spin.setValue(1.0)
        page.power_min_spin.setValue(0)
        page.power_spin.setValue(800)
        page.threshold_enabled_check.setChecked(False)
        page.raster_pwm_check.setChecked(True)
        page.set_image("grayscale.png", image)

        page.btn_convert.click()

        powers = {
            int(token[1:])
            for line in page.editor.toPlainText().splitlines()
            for token in line.split()
            if token.startswith("S") and token[1:].isdigit()
        }
        positive_powers = {power for power in powers if power > 0}
        self.assertIn(800, positive_powers)
        self.assertGreaterEqual(len(positive_powers), 4)
        self.assertTrue(any(0 < power < 800 for power in positive_powers))
        self.assertIn("S0-S800", page.image_status.text())

    def test_transparent_pixels_are_composited_on_white_before_conversion(self) -> None:
        page = GcodePage()
        image = QImage(2, 1, QImage.Format.Format_ARGB32)
        image.setPixelColor(0, 0, QColor(0, 0, 0, 0))
        image.setPixelColor(1, 0, QColor(0, 0, 0, 255))

        self.assertEqual(
            [list(row) for row in page._grayscale_rows_from_image(image)],
            [[255, 0]],
        )

        page.resize_mode_combo.setCurrentIndex(
            page.resize_mode_combo.findData("nearest")
        )
        page.size_spin.setValue(2.0)
        page.target_height_spin.setValue(1.0)
        page.scan_quality_spin.setValue(1.0)
        page.threshold_enabled_check.setChecked(False)
        page.raster_pwm_check.setChecked(True)
        page.set_image("transparent.png", image)
        page.btn_convert.click()

        self.assertEqual(
            (page._converted_image.width(), page._converted_image.height()),
            (2, 1),
        )
        self.assertEqual(page._converted_image.pixelColor(0, 0), QColor("white"))
        self.assertEqual(page._converted_image.pixelColor(1, 0), QColor("black"))

    def test_vector_conversion_uses_quality_above_legacy_320_pixel_cap(self) -> None:
        page = GcodePage()
        image = QImage(20, 10, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        page.size_spin.setValue(30.0)
        page.target_height_spin.setValue(30.0)
        page.vector_quality_spin.setValue(12.0)
        page.extract_mode.setCurrentIndex(
            page.extract_mode.findData("lasergrbl_vector")
        )
        page.set_image("high_quality_vector.png", image)

        page.btn_convert.click()

        self.assertEqual(
            (page._converted_image.width(), page._converted_image.height()),
            (361, 181),
        )
        self.assertGreater(page._converted_image.width(), 320)

    def test_grayscale_preview_maps_power_to_white_gray_and_black(self) -> None:
        preview = GcodePage._build_grayscale_preview(
            [[0, 500, 1000]],
            max_power=1000,
        )

        shades = [preview.pixelColor(x, 0).red() for x in range(3)]
        self.assertEqual(shades[0], 255)
        self.assertGreater(shades[1], 0)
        self.assertLess(shades[1], 255)
        self.assertEqual(shades[2], 0)

    def test_vector_fill_preview_renders_fill_mask(self) -> None:
        preview = GcodePage._build_vector_fill_preview(
            [[True, False], [False, True]],
            [],
            2,
            2,
        )

        self.assertEqual((preview.width(), preview.height()), (3, 3))
        self.assertEqual(preview.pixelColor(0, 0), QColor("#cbd5e1"))
        self.assertEqual(preview.pixelColor(1, 0), QColor("white"))
        self.assertEqual(preview.pixelColor(1, 1), QColor("#cbd5e1"))

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

    def test_extreme_portrait_uses_selected_scan_quality_without_file_cap(self) -> None:
        page = GcodePage()
        image = QImage(1, 1000, QImage.Format.Format_RGB32)
        image.fill(QColor("black"))
        emitted: list[tuple[str, bytes]] = []
        page.gcode_loaded.connect(lambda path, data: emitted.append((path, data)))

        page.set_image("portrait.png", image)
        page.btn_convert.click()

        scan_rows = sum(
            line.startswith(b"G0 ") for line in emitted[-1][1].splitlines()
        )
        self.assertEqual(scan_rows, 990)
        self.assertIn("10.0 线/mm", page.image_status.text())

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

    def test_focus_power_config_rejects_zero_by_clamping_to_minimum(self) -> None:
        self.assertEqual(HostConfig(focus_power=0).focus_power, 1)
        self.assertEqual(HostConfig(focus_power=101).focus_power, 100)

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
            completed_lines=0, total_lines=100,
            focus="OFF", tx_link="CONNECTED", rx_link="CONNECTED",
        )
        self.assertEqual(page.arc._value, 50)
        self.assertEqual(page.arc._caption, "下载进度")
        self.assertIn("50/100", page.lbl_substate.text())

        page.update_state(
            rx_state="正在执行", rx_state_code=3, job_id=1,
            received=100, total=100, cache_free=1000,
            completed_lines=25, total_lines=100,
            focus="OFF", tx_link="CONNECTED", rx_link="CONNECTED",
        )
        self.assertEqual(page.arc._value, 0)
        self.assertEqual(page.arc._caption, "正在执行")
        self.assertEqual(page.lbl_substate.text(), "任务正在执行")
        self.assertEqual(page.progress.value(), 1000)
        self.assertEqual(page._exec_progress_animation.endValue(), 250)
        self.assertIn("25/100 行", page.lbl_exec_progress_pct.text())

    def test_preroll_execution_visual_survives_receiving_status_refresh(self) -> None:
        page = JobPage()
        page.set_execution_state(True)

        page.update_state(
            rx_state="数据传输中", rx_state_code=1, job_id=7,
            received=6000, total=12000, cache_free=96400,
            completed_lines=0, total_lines=1000,
            focus="OFF", tx_link="CONNECTED", rx_link="CONNECTED",
        )

        self.assertTrue(page.arc._spinning)
        self.assertEqual(page.lbl_state.text(), "正在执行")
        self.assertEqual(page.arc._caption, "等待 RX 完成")
        self.assertEqual(page.cards["已接收字节"].text(), "6000 字节")

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

    def test_unsolicited_progress_updates_state_without_rendering_log(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        rendered: list[tuple[str, str]] = []
        window.page_logs.append_log = lambda channel, message: rendered.append((channel, message))

        window._on_log_message(
            "protocol",
            "@PROGRESS state=3 status=0 job=9 rx=600 total=1000 free=500 "
            "lines=70 completed=50 total_lines=100",
        )

        self.assertEqual(window.state.rx_received, 600)
        self.assertEqual(window.state.rx_completed_lines, 50)
        self.assertEqual(window.state.rx_total_lines, 100)
        self.assertFalse(any(channel == "protocol" for channel, _message in rendered))

    def test_same_job_progress_cannot_move_backwards(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.state.rx_job_id = 9
        window.state.rx_received = 600
        window.state.rx_completed_lines = 50
        window.state.rx_total_lines = 100

        window._parse_rx_line(
            "@PROGRESS state=3 status=0 job=9 rx=500 total=1000 free=500 "
            "lines=60 completed=40 total_lines=100"
        )

        self.assertEqual(window.state.rx_received, 600)
        self.assertEqual(window.state.rx_completed_lines, 50)

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

    def test_cancelled_task_is_held_then_reset_to_idle(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.state.rx_state_code = 3
        window.state.rx_job_id = 7
        window.state.rx_received = 23100
        window.state.rx_job_total = 74856
        window.state.rx_completed_lines = 281
        window.state.rx_total_lines = 4299
        window.state.execution_expected = True

        window._on_task_status("已取消并清空", -1)

        self.assertTrue(window._cancelled_task_timer.isActive())
        self.assertEqual(window.page_job.lbl_state.text(), "已取消")
        self.assertEqual(window.state.rx_state_code, 5)

        window._return_cancelled_task_to_idle()

        self.assertFalse(window._cancelled_task_timer.isActive())
        self.assertEqual(window.state.rx_state_code, 0)
        self.assertEqual(window.state.rx_job_id, 0)
        self.assertEqual(window.state.rx_received, 0)
        self.assertEqual(window.state.rx_job_total, 0)
        self.assertEqual(window.page_job.lbl_state.text(), "空闲")
        self.assertEqual(window.page_job.lbl_task.text(), "空闲")
        self.assertFalse(window.state.execution_complete)
        self.assertFalse(window.state.termination_requested)

    def test_post_cancel_executing_status_cannot_restore_spinner(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        window.state.rx_state_code = 3
        window.state.rx_job_id = 7
        window.state.execution_expected = True
        window._on_task_status("已取消并清空", -1)
        window._return_cancelled_task_to_idle()

        window._parse_rx_line(
            "@STATUS state=3 status=0 job=7 rx=23100 total=74856 "
            "free=85529 lines=281 completed=281 total_lines=4299"
        )

        self.assertEqual(window.state.rx_state_code, 0)
        self.assertFalse(window.state.execution_expected)
        self.assertFalse(window.page_job.arc._spinning)

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

    def test_exec_status_poll_skips_when_command_path_is_busy(self) -> None:
        window = MainWindow()
        self.addCleanup(window.close)
        calls: list[float] = []

        class FakeClient:
            def try_transact_status(self, timeout: float):
                calls.append(timeout)
                return None

            def transact_status(self, timeout: float):
                raise AssertionError("blocking status path must not be used")

            def close(self) -> None:
                pass

        window.client = FakeClient()
        window.state.execution_expected = True
        window._exec_poll_timer.start()

        window._execution_status_poll_worker()

        self.assertEqual(calls, [6.0])
        self.assertEqual(window._exec_poll_failures, 0)

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
