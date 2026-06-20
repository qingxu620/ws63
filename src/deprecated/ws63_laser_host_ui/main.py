from __future__ import annotations

import sys

from PySide6.QtWidgets import QApplication

from app.config_store import ConfigStore
from app.event_bus import EventBus
from ui.main_window import MainWindow


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("WS63 Laser Host")
    app.setOrganizationName("fbb_ws63")

    config_store = ConfigStore()
    event_bus = EventBus()
    window = MainWindow(config_store=config_store, event_bus=event_bus)
    window.resize(1280, 780)
    window.show()

    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
