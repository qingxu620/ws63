"""
WS63 Laser Host V2 — PySide6 presentation GUI for SLE Job protocol.

Architecture:
  app/           Core: event bus, config, state models
  transports/    Serial protocol client (SleJobSerialClient) and log monitors
  workers/       QThread wrappers for non-blocking serial operations
  ui/            PySide6 pages, widgets, and main window

Protocol chain:
  PC Host  ->  USB UART  ->  TX Board  ->  SLE  ->  RX Board  ->  Laser
"""
from __future__ import annotations

import sys

from PySide6.QtWidgets import QApplication

from ui.main_window import MainWindow


def _apply_global_style(app: QApplication) -> None:
    app.setStyleSheet("""
        * { font-family: "Outfit", "Segoe UI", "Microsoft YaHei", sans-serif; }

        QMainWindow { background: #f8fafc; color: #0f172a; }

        QWidget#statusStrip {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #ffffff, stop:1 #f8fafc);
            border-bottom: 1px solid #e2e8f0;
        }
        QLabel#headerBrand {
            color: #0284c7; background: rgba(2, 132, 199, 0.05);
            border: 1px solid rgba(2, 132, 199, 0.14); border-radius: 13px;
            padding: 5px 12px; font-size: 11px; font-weight: 700;
            letter-spacing: 0.3px;
        }

        QWidget#sidebar {
            background: #f1f5f9;
            border-right: 1px solid #e2e8f0;
        }

        QPushButton#sidebarBtn {
            background: transparent; color: #475569;
            border: none; border-radius: 8px;
            padding: 13px 16px; text-align: left;
            font-size: 14px; font-weight: 600;
        }
        QPushButton#sidebarBtn:hover { background: rgba(0, 0, 0, 0.03); color: #0f172a; }
        QPushButton#sidebarBtn:checked {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 rgba(2, 132, 199, 0.08), stop:1 rgba(2, 132, 199, 0.01));
            color: #0284c7; border-left: 3px solid #0284c7;
        }

        QPushButton#sidebarBrand {
            background: transparent; color: #94a3b8;
            border: none; font-size: 11px; font-weight: 700;
            text-transform: uppercase; letter-spacing: 2px;
            padding: 12px 16px;
        }

        QLabel#pageSubtitle {
            font-size: 14px; color: #475569; font-weight: 400;
        }

        QTabWidget#gcodeTabs::pane {
            border: none; border-top: 1px solid #e2e8f0; top: -1px;
        }
        QTabWidget#gcodeTabs QTabBar::tab {
            background: #ffffff; color: #475569;
            border: 1px solid #e2e8f0; border-radius: 7px;
            padding: 9px 16px; margin-right: 8px; font-weight: 600;
        }
        QTabWidget#gcodeTabs QTabBar::tab:selected {
            background: #0284c7; color: #ffffff; border-color: #0284c7;
        }
        QLabel#taskLabel {
            font-size: 14px; font-weight: 600; color: #0f172a;
        }
        QLabel#stateLabel {
            font-size: 24px; font-weight: 800; color: #0284c7;
        }
        QLabel#substateLabel {
            font-size: 13px; color: #475569; font-weight: 500;
            letter-spacing: 0.5px; text-transform: uppercase;
        }

        QGroupBox {
            background: #ffffff; color: #0f172a;
            border: 1px solid #e2e8f0; border-radius: 12px;
            margin-top: 16px; padding: 0px;
            font-size: 13px; font-weight: 700;
        }
        QGroupBox::title {
            subcontrol-origin: margin; left: 16px; padding: 0 10px;
            color: #0284c7; border-left: 3px solid #0284c7;
            font-size: 13px; font-weight: 700;
            text-transform: uppercase; letter-spacing: 1px;
        }

        QPushButton {
            background: #ffffff; color: #0f172a;
            border: 1px solid #e2e8f0; border-radius: 8px;
            padding: 8px 16px; font-size: 13px; font-weight: 600;
        }
        QPushButton:hover { background: #f8fafc; border-color: #cbd5e1; }
        QPushButton:pressed { background: #e2e8f0; }
        QPushButton:disabled { background: #f1f5f9; color: #94a3b8; border-color: #e2e8f0; }

        QPushButton#btnPrimary {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #0284c7, stop:1 #0369a1);
            border: 1px solid #0284c7; color: white;
        }
        QPushButton#btnPrimary:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #0369a1, stop:1 #075985);
            border-color: #0369a1;
        }

        QPushButton#btnAccent {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #10b981, stop:1 #059669);
            border: 1px solid #10b981; color: white;
        }
        QPushButton#btnAccent:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #059669, stop:1 #047857);
            border-color: #059669;
        }

        QPushButton#btnFocus {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #10b981, stop:1 #059669);
            border: 1px solid #10b981; color: white;
        }
        QPushButton#btnFocus:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #059669, stop:1 #047857);
            border-color: #059669;
        }
        QPushButton#btnFocus[active="true"] {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #ef4444, stop:1 #dc2626);
            border: 1px solid #ef4444; color: white;
        }
        QPushButton#btnFocus[active="true"]:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #dc2626, stop:1 #b91c1c);
            border-color: #dc2626;
        }

        QPushButton#btnWarn {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #f59e0b, stop:1 #d97706);
            border: 1px solid #f59e0b; color: white;
        }
        QPushButton#btnWarn:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #d97706, stop:1 #b45309);
            border-color: #d97706;
        }

        QPushButton#btnDanger {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #ef4444, stop:1 #dc2626);
            border: 1px solid #ef4444; color: white;
        }
        QPushButton#btnDanger:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #dc2626, stop:1 #b91c1c);
            border-color: #dc2626;
        }

        QLineEdit, QComboBox {
            background: #ffffff; color: #0f172a;
            border: 1px solid #e2e8f0; border-radius: 8px;
            padding: 8px 12px; font-size: 14px;
        }
        QLineEdit:focus, QComboBox:focus { border-color: #0284c7; background: #ffffff; }
        QComboBox::drop-down {
            border: none; padding-right: 12px;
        }
        QComboBox QAbstractItemView {
            background: #ffffff; color: #0f172a;
            border: 1px solid #e2e8f0; selection-background-color: #f1f5f9;
            selection-color: #0284c7; padding: 4px;
        }

        QProgressBar {
            background: #f1f5f9; border: 1px solid #e2e8f0;
            border-radius: 5px; text-align: center;
            color: #475569; font-size: 11px; font-weight: 600;
            height: 10px;
        }
        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #0284c7, stop:1 #10b981);
            border-radius: 4px;
        }

        QCheckBox { color: #475569; font-size: 13px; font-weight: 600; spacing: 8px; }
        QCheckBox::indicator {
            width: 16px; height: 16px;
            border: 1px solid #e2e8f0; border-radius: 4px;
            background: #ffffff;
        }
        QCheckBox::indicator:hover {
            border-color: #0284c7;
        }
        QCheckBox::indicator:checked {
            background: #0284c7; border: 3px solid #ffffff; border-radius: 4px;
        }

        QTextEdit {
            background: #ffffff; color: #334155;
            border: 1px solid #e2e8f0; border-radius: 12px;
            font-family: "Fira Code", "Cascadia Code", "Consolas", monospace;
            font-size: 12.5px; line-height: 1.5; padding: 16px;
        }

        QFrame#monitorCard {
            background: #ffffff; border: 1px solid #e2e8f0;
            border-radius: 10px; padding: 6px;
        }
        QLabel#cardLabel { color: #475569; font-size: 11px; font-weight: 600; text-transform: uppercase; letter-spacing: 1px; }
        QLabel#cardValue { color: #0f172a; font-size: 20px; font-weight: 700; }

        QScrollBar:vertical {
            background: transparent; width: 8px; border: none; margin: 0px;
        }
        QScrollBar::handle:vertical {
            background: rgba(0, 0, 0, 0.08); border-radius: 4px; min-height: 30px;
        }
        QScrollBar::handle:vertical:hover { background: rgba(0, 0, 0, 0.15); }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    """)


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("WS63 Laser Host")
    app.setOrganizationName("fbb_ws63")
    _apply_global_style(app)

    window = MainWindow()
    window.resize(1280, 800)
    window.show()

    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
