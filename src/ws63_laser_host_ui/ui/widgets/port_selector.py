from __future__ import annotations

from PySide6.QtWidgets import QComboBox

from utils.serial_ports import SerialPortInfo


class PortSelector(QComboBox):
    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setEditable(True)

    def set_ports(self, ports: list[SerialPortInfo], current_device: str = "") -> None:
        current = current_device or self.device()
        self.blockSignals(True)
        self.clear()
        for port in ports:
            self.addItem(port.display_name, port.device)
        if current:
            self.set_device(current)
        self.blockSignals(False)

    def set_device(self, device: str) -> None:
        if not device:
            self.setCurrentText("")
            return
        for index in range(self.count()):
            if self.itemData(index) == device:
                self.setCurrentIndex(index)
                return
        self.addItem(device, device)
        self.setCurrentIndex(self.count() - 1)

    def device(self) -> str:
        data = self.currentData()
        if isinstance(data, str) and data:
            return data
        text = self.currentText().strip()
        if " - " in text:
            return text.split(" - ", 1)[0].strip()
        return text
